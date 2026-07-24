/**
 * @file kernel/proc/execve.cpp
 * @brief execve() implementation — ELF loading and address space replacement
 *
 * Reads an ELF binary from the VFS, validates headers, unmaps existing
 * user-space pages, loads PT_LOAD segments into the task's address
 * space, and sets the new entry point.  Also contains the helper that
 * converts ELF validation errors to ExecveResult codes.
 */

#include <stddef.h>
#include <stdint.h>

#include <new>

#include "kernel/arch/x86_64/memory_layout.hpp"
#include "kernel/arch/x86_64/paging.hpp"
#include "kernel/arch/x86_64/paging_config.hpp"
#include "kernel/arch/x86_64/usermode.hpp"  // F9 batch 1: USER_SIGRETURN_PAGE
#include "kernel/fs/inode_ref.hpp"          // InodeRef: RAII over the lookup ref
#include "kernel/fs/vfs_lookup.hpp"         // F-USABILITY batch 1c: symlink-following lookup
#include "kernel/lib/aslr.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/proc/elf_load.hpp"  // F10-M2: load_elf_image (main + interp)
#include "kernel/proc/elf_types.hpp"
#include "kernel/proc/process.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/signal.hpp"  // F9 batch 1: kSigreturnTrampoline

namespace cinux::proc {

// ============================================================
// Internal helpers for execve
// ============================================================

namespace {

ExecveResult elf_error_to_execve(elf::ElfValidateResult r) {
    using ER = elf::ElfValidateResult;
    switch (r) {
    case ER::BadMagic:
        return ExecveResult::BadElfMagic;
    case ER::BadClass:
        return ExecveResult::BadElfClass;
    case ER::BadEndian:
        return ExecveResult::BadElfEndian;
    case ER::BadMachine:
        return ExecveResult::BadElfMachine;
    case ER::BadType:
        return ExecveResult::BadElfType;
    case ER::BadPhoff:
    case ER::BadPhdrSize:
    case ER::BadPhnum:
    case ER::NoPhdrs:
        return ExecveResult::BadElfHeaders;
    default:
        return ExecveResult::BadElfHeaders;
    }
}

/**
 * @brief Unmap all user-space pages from an address space
 *
 * Walks PML4 entries 0..255 and frees every mapped data page and
 * page table page.  Does NOT free the PML4 itself.
 */
void clear_user_mappings(cinux::mm::AddressSpace& space) {
    uint64_t pml4_phys = space.pml4_phys();
    auto*    pml4 =
        reinterpret_cast<cinux::arch::PageEntry*>(pml4_phys + cinux::arch::DIRECT_MAP_BASE);

    for (uint32_t i = 0; i < 256; i++) {
        if (!pml4[i].is_present())
            continue;

        auto* pdpt = reinterpret_cast<cinux::arch::PageEntry*>(pml4[i].phys_addr() +
                                                               cinux::arch::DIRECT_MAP_BASE);

        for (uint32_t j = 0; j < cinux::arch::PT_ENTRIES; j++) {
            if (!pdpt[j].is_present())
                continue;

            // DEBT-009: a 1 GB huge PDPTE maps a data page, not a PD table --
            // descending would parse the huge-page body as PD/PT entries and
            // free garbage.  Huge-page free (buddy order) isn't wired yet
            // (no user huge mappings); clear the entry and skip.  Hitting
            // this means a future huge-mapping milestone forgot this path.
            if (pdpt[j].huge) {
                cinux::lib::kprintf(
                    "[EXEC] clear_user_mappings: 1GB huge phys=0x%lx "
                    "skipped (huge free unimplemented)\n",
                    static_cast<unsigned long>(pdpt[j].phys_addr()));
                pdpt[j].raw = 0;
                continue;
            }

            auto* pd = reinterpret_cast<cinux::arch::PageEntry*>(pdpt[j].phys_addr() +
                                                                 cinux::arch::DIRECT_MAP_BASE);

            for (uint32_t k = 0; k < cinux::arch::PT_ENTRIES; k++) {
                if (!pd[k].is_present())
                    continue;

                // DEBT-009: a 2 MB huge PDE maps a data page, not a PT table.
                // See the PDPT-level note above -- skip, don't descend.
                if (pd[k].huge) {
                    cinux::lib::kprintf(
                        "[EXEC] clear_user_mappings: 2MB huge phys=0x%lx "
                        "skipped (huge free unimplemented)\n",
                        static_cast<unsigned long>(pd[k].phys_addr()));
                    pd[k].raw = 0;
                    continue;
                }

                auto* pt = reinterpret_cast<cinux::arch::PageEntry*>(pd[k].phys_addr() +
                                                                     cinux::arch::DIRECT_MAP_BASE);

                for (uint32_t l = 0; l < cinux::arch::PT_ENTRIES; l++) {
                    if (!pt[l].is_present())
                        continue;
                    // Device/IoPhys pages (FLAG_PCD) are not PMM-managed.
                    if (pt[l].raw & cinux::arch::FLAG_PCD) {
                        pt[l].raw = 0;
                        continue;
                    }
                    uint64_t data_phys = pt[l].phys_addr();
                    cinux::mm::g_pmm.pte_count_dec_and_test(data_phys);
                    pt[l].raw = 0;
                }

                uint64_t pt_phys = pd[k].phys_addr();
                cinux::mm::g_pmm.free_page(pt_phys);
                pd[k].raw = 0;
            }

            uint64_t pd_phys = pdpt[j].phys_addr();
            cinux::mm::g_pmm.free_page(pd_phys);
            pdpt[j].raw = 0;
        }

        uint64_t pdpt_phys = pml4[i].phys_addr();
        cinux::mm::g_pmm.free_page(pdpt_phys);
        pml4[i].raw = 0;
    }
}

}  // anonymous namespace

// ============================================================
// execve implementation
// ============================================================

ExecveResult execve(const char* path, [[maybe_unused]] const char* const argv[],
                    [[maybe_unused]] const char* const envp[], ElfAuxInfo* aux_out) {
    using namespace cinux::arch;


    if (path == nullptr || path[0] == '\0') {
        cinux::lib::kprintf("[EXECVE] invalid path\n");
        return ExecveResult::BadPath;
    }

    auto* task = Scheduler::current();
    if (task == nullptr) {
        cinux::lib::kprintf("[EXECVE] no current task\n");
        return ExecveResult::NoCurrentTask;
    }

    if (task->addr_space == nullptr) {
        cinux::lib::kprintf("[EXECVE] task has no address space\n");
        return ExecveResult::NoAddressSpace;
    }

    // F-USABILITY batch 1c: vfs_lookup follows symlinks (e.g. /sbin/init ->
    // /bin/busybox), replacing the open-coded vfs_resolve + fs->lookup that
    // stopped at the link and failed with "not a regular file".
    const char* cwd = (task->cwd != nullptr) ? task->cwd->path : "/";
    auto        lr =
        cinux::fs::vfs_lookup(path, static_cast<uint32_t>(cinux::fs::LookupFlag::Follow), cwd);
    if (!lr.ok()) {
        cinux::lib::kprintf("[EXECVE] lookup failed: %s\n", path);
        return ExecveResult::FileNotFound;
    }
    cinux::fs::InodeRef inode(lr.value().target);  // RAII: lookup ref dropped at every return

    if (inode->type != cinux::fs::InodeType::Regular) {
        cinux::lib::kprintf("[EXECVE] not a regular file: %s\n", path);
        return ExecveResult::FileNotRegular;
    }

    constexpr uint64_t ELF_HEADER_SIZE = sizeof(elf::Elf64_Ehdr);
    if (inode->size < ELF_HEADER_SIZE) {
        cinux::lib::kprintf("[EXECVE] file too small for ELF header: %lu bytes\n",
                            static_cast<unsigned long>(inode->size));
        return ExecveResult::ReadFailed;
    }

    uint8_t ehdr_buf[ELF_HEADER_SIZE];
    if (inode->ops == nullptr) {
        cinux::lib::kprintf("[EXECVE] inode has no ops\n");
        return ExecveResult::ReadFailed;
    }

    auto nread = inode->ops->read(inode.get(), 0, ehdr_buf, ELF_HEADER_SIZE);
    if (!nread.ok() || nread.value() < static_cast<int64_t>(ELF_HEADER_SIZE)) {
        cinux::lib::kprintf("[EXECVE] failed to read ELF header\n");
        return ExecveResult::ReadFailed;
    }

    auto* ehdr = reinterpret_cast<const elf::Elf64_Ehdr*>(ehdr_buf);

    auto vr = elf::validate_elf_header(ehdr, inode->size);
    if (vr != elf::ElfValidateResult::Ok) {
        cinux::lib::kprintf("[EXECVE] ELF validation failed: %d\n", static_cast<int>(vr));
        return elf_error_to_execve(vr);
    }

    uint64_t phdr_offset = ehdr->e_phoff;
    uint16_t phnum       = ehdr->e_phnum;
    uint64_t phdr_bytes  = static_cast<uint64_t>(phnum) * sizeof(elf::Elf64_Phdr);
    auto*    phdrs       = new (std::align_val_t{alignof(elf::Elf64_Phdr)}) elf::Elf64_Phdr[phnum];
    if (phdrs == nullptr) {
        cinux::lib::kprintf("[EXECVE] failed to allocate phdr buffer\n");
        return ExecveResult::MapFailed;
    }

    nread = inode->ops->read(inode.get(), phdr_offset, phdrs, phdr_bytes);
    if (!nread.ok() || nread.value() < static_cast<int64_t>(phdr_bytes)) {
        cinux::lib::kprintf("[EXECVE] failed to read program headers\n");
        delete[] phdrs;
        return ExecveResult::ReadFailed;
    }

    if (task->vfork_parent != nullptr) {
        auto* new_space = new cinux::mm::AddressSpace();
        if (new_space == nullptr) {
            cinux::lib::kprintf("[EXECVE] vfork address-space allocation failed\n");
            return ExecveResult::MapFailed;
        }
        auto* old_space  = task->addr_space;
        task->addr_space = new_space;
        if (old_space != nullptr && old_space->release()) {
            delete old_space;
        }
        task->addr_space->activate();
    } else {
        clear_user_mappings(*task->addr_space);
        // F10 shell-launch: execve replaces the image, so discard any VMA records
        // inherited from fork (a fresh AddressSpace has none; a forked child has the
        // parent's).  Without this the new PT_LOAD insert collides with the stale
        // set and "VMA record failed" aborts the load.
        task->addr_space->vmas().clear();
    }

    // Choose the main program's load base. ET_EXEC (non-PIE) p_vaddr is an
    // absolute address, so base 0 honours it verbatim. ET_DYN (PIE) p_vaddr is
    // base-relative, so we pick USER_EXEC_BASE plus a small page-aligned ASLR
    // offset -- the main-program counterpart of the interpreter's fixed
    // USER_INTERP_BASE. load_elf_image adds base to each p_vaddr and returns
    // the phdr VA, brk end, and entry for this image.
    const uint64_t main_base =
        (ehdr->e_type == elf::ET_DYN)
            ? (cinux::arch::USER_EXEC_BASE + cinux::lib::aslr_exec_base_offset())
            : 0;
    LoadedImage  main_img{};
    ExecveResult load_res =
        load_elf_image(*task->addr_space, inode.get(), ehdr, phdrs, phnum, main_base, main_img);
    if (load_res != ExecveResult::Ok) {
        delete[] phdrs;
        return load_res;
    }

    // F10-M2: detect a dynamic executable. PT_INTERP names the dynamic linker
    // (e.g. /lib/ld-musl-x86_64.so.1); the path is a NUL-terminated string in
    // the main file at p_offset, length p_filesz. The kernel loads that
    // interpreter and hands off to it -- GOT/PLT/DT_NEEDED are the ldso's job.
    char interp_path[256];
    bool has_interp = false;
    for (uint16_t i = 0; i < phnum; i++) {
        if (phdrs[i].p_type != elf::PT_INTERP) {
            continue;
        }
        uint64_t plen = phdrs[i].p_filesz;
        if (plen == 0 || plen >= sizeof(interp_path)) {
            cinux::lib::kprintf("[EXECVE] PT_INTERP bad length %lu\n",
                                static_cast<unsigned long>(plen));
            delete[] phdrs;
            return ExecveResult::BadElfHeaders;
        }
        auto pread = inode->ops->read(inode.get(), phdrs[i].p_offset, interp_path, plen);
        if (!pread.ok() || pread.value() < static_cast<int64_t>(plen)) {
            cinux::lib::kprintf("[EXECVE] PT_INTERP path read failed\n");
            delete[] phdrs;
            return ExecveResult::ReadFailed;
        }
        interp_path[plen] = '\0';  // PT_INTERP usually includes the NUL; force it.
        has_interp        = true;
        break;
    }

    // F4-B0: scan PT_GNU_STACK to decide whether the user stack may be executable.
    // glibc-built ELFs carry PT_GNU_STACK=RW (NX stack, the glibc default); only
    // legacy RWX marks request an executable stack. Absence -> NX (modern default;
    // Linux defaults to X only for ABI-legacy binaries Cinux does not host).
    constexpr uint32_t kPtGnuStack      = 0x6474e551;
    bool               stack_executable = false;
    for (uint16_t i = 0; i < phnum; i++) {
        if (phdrs[i].p_type == kPtGnuStack && (phdrs[i].p_flags & elf::PF_X)) {
            stack_executable = true;
            break;
        }
    }

    delete[] phdrs;

    if (!main_img.has_load) {
        cinux::lib::kprintf("[EXECVE] no PT_LOAD segments found\n");
        return ExecveResult::NoLoadSegments;
    }

    // F2-M3: initialise the user heap. brk starts at the page-aligned end of
    // the ELF image; the Heap VMA spans [brk_initial, brk_max) so demand
    // paging services heap growth without further bookkeeping.
    //
    // F9 batch 8 (ASLR): add a page-aligned random gap above the image so the
    // heap start moves per-exec. PIE batch 1: brk_max follows the image. A
    // low-loaded main (ET_EXEC, main_base 0) keeps USER_BRK_MAX as the ceiling
    // (the gap below the interpreter at USER_INTERP_BASE = 256 MB); a high-
    // loaded PIE main (main_base >= USER_EXEC_BASE) cannot reach USER_BRK_MAX,
    // so its heap runs up to the mmap region instead. Without this, a PIE
    // image ending above USER_BRK_MAX makes [brk_initial, USER_BRK_MAX) an
    // inverted range and the VMA record fails.
    const uint64_t brk_max   = (main_base < cinux::arch::USER_INTERP_BASE)
                                   ? cinux::arch::USER_BRK_MAX
                                   : cinux::arch::USER_MMAP_BASE;
    uint64_t       brk_start = (main_img.max_seg_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint64_t       brk_gap   = cinux::lib::aslr_brk_offset();
    if (brk_start + brk_gap >= brk_max) {
        brk_gap = 0;
    }
    task->brk_initial = brk_start + brk_gap;
    task->brk_current = task->brk_initial;
    task->brk_max     = brk_max;
    constexpr cinux::mm::VmaFlags kHeapVma =
        cinux::mm::VmaFlags::Read | cinux::mm::VmaFlags::Write | cinux::mm::VmaFlags::Heap;
    if (!task->addr_space->vmas().insert(task->brk_initial, task->brk_max, kHeapVma).ok()) {
        cinux::lib::kprintf("[EXECVE] heap VMA record failed\n");
        return ExecveResult::MapFailed;
    }

    // F10-M2: load the dynamic interpreter when PT_INTERP was present. The
    // interpreter (ld-musl / ld-linux) is mapped at USER_INTERP_BASE and we
    // enter IT -- after relocating the main program it jumps to AT_ENTRY (the
    // main entry). A static executable (no PT_INTERP) enters the main entry
    // directly, unchanged from before.
    uint64_t interp_base = 0;
    // Static default: enter the main program directly. For ET_EXEC the entry is
    // absolute (main_base == 0); for a static-PIE ET_DYN it is base-relative,
    // so add main_base here too. A dynamic executable overrides entry_va below
    // with the interpreter's entry (the ldso relocates main, then jumps to
    // AT_ENTRY).
    uint64_t entry_va    = main_base + ehdr->e_entry;
    if (has_interp) {
        ExecveResult ir = load_interpreter(*task->addr_space, interp_path, interp_base, entry_va);
        if (ir != ExecveResult::Ok) {
            return ir;
        }
    }
    task->ctx.rip = entry_va;

    // F9 batch 1: map the fixed sigreturn trampoline page.  The handler return
    // address (set in signal_setup_frame) lands here, keeping the int $0x80
    // sigreturn stub off the user stack so the stack can be NX (F9 batch 2).
    // Read-only + user (executable: no FLAG_NX).  Aligned with Linux vDSO.
    {
        uint64_t sigret_phys = cinux::mm::g_pmm.alloc_page();
        if (sigret_phys == 0) {
            cinux::lib::kprintf("[EXECVE] sigreturn page alloc failed\n");
            return ExecveResult::MapFailed;
        }
        auto* dst = reinterpret_cast<uint8_t*>(sigret_phys + cinux::arch::DIRECT_MAP_BASE);
        for (uint64_t b = 0; b < cinux::arch::PAGE_SIZE; b++) {
            dst[b] = 0;
        }
        for (int i = 0; i < 8; i++) {
            dst[i] = cinux::proc::kSigreturnTrampoline[i];
        }
        constexpr uint64_t kSigretFlags = cinux::arch::FLAG_PRESENT | cinux::arch::FLAG_USER;
        if (!task->addr_space->map(cinux::arch::USER_SIGRETURN_PAGE, sigret_phys, kSigretFlags)) {
            cinux::lib::kprintf("[EXECVE] sigreturn page map failed\n");
            cinux::mm::g_pmm.refcount_dec_and_test(sigret_phys);  // batch 4: roll back alloc
            return ExecveResult::MapFailed;
        }
        cinux::mm::g_pmm.pte_count_inc(sigret_phys);  // batch 3: account for the installed PTE
    }

    if (aux_out != nullptr) {
        aux_out->at_phdr          = main_img.phdr_va;
        aux_out->at_phnum         = ehdr->e_phnum;
        aux_out->at_phent         = sizeof(elf::Elf64_Phdr);
        // AT_ENTRY is the main program's real entry VA. ET_DYN entries are
        // base-relative, so add main_base; for ET_EXEC main_base is 0 (no-op).
        // The ldso relocates the main image then jumps here.
        aux_out->at_entry         = main_base + ehdr->e_entry;
        aux_out->at_base          = interp_base;  // interpreter load base (0 if static)
        aux_out->has_interp       = has_interp;
        aux_out->stack_executable = stack_executable;  // F4-B0: PT_GNU_STACK PF_X
    }

    if (task->vfork_parent != nullptr) {
        Scheduler::unblock(task->vfork_parent);
        task->vfork_parent = nullptr;
    }

    return ExecveResult::Ok;
}

}  // namespace cinux::proc
