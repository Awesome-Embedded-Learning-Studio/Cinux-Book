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
#include "kernel/fs/vfs_mount.hpp"
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

                    uint64_t data_phys = pt[l].phys_addr();
                    // Q4b-2 (DEBT-003): only free if this was the last mapping.
                    // A CoW-shared page (fork) has mapcount > 1 here; freeing
                    // it would leave the other process's PTE dangling (UAF).
                    if (cinux::mm::g_pmm.mapcount_dec_and_test(data_phys)) {
                        cinux::mm::g_pmm.free_page(data_phys);
                    }
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

ExecveResult execve(const char* path, const char* const argv[], const char* const envp[],
                    ElfAuxInfo* aux_out) {
    using namespace cinux::arch;

    (void)argv;
    (void)envp;

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

    const char* rel_path = nullptr;
    auto*       fs       = cinux::fs::vfs_resolve(path, &rel_path);
    if (fs == nullptr) {
        cinux::lib::kprintf("[EXECVE] path not found: %s\n", path);
        return ExecveResult::FileNotFound;
    }

    auto lookup_result = fs->lookup(rel_path);
    if (!lookup_result.ok()) {
        cinux::lib::kprintf("[EXECVE] inode not found: %s\n", rel_path);
        return ExecveResult::FileNotFound;
    }
    auto* inode = lookup_result.value();

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

    auto nread = inode->ops->read(inode, 0, ehdr_buf, ELF_HEADER_SIZE);
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

    nread = inode->ops->read(inode, phdr_offset, phdrs, phdr_bytes);
    if (!nread.ok() || nread.value() < static_cast<int64_t>(phdr_bytes)) {
        cinux::lib::kprintf("[EXECVE] failed to read program headers\n");
        delete[] phdrs;
        return ExecveResult::ReadFailed;
    }

    clear_user_mappings(*task->addr_space);
    // F10 shell-launch: execve replaces the image, so discard any VMA records
    // inherited from fork (a fresh AddressSpace has none; a forked child has the
    // parent's).  Without this the new PT_LOAD insert collides with the stale
    // set and "VMA record failed" aborts the load.
    task->addr_space->vmas().clear();

    // Map the main program's PT_LOAD segments (base 0: non-PIE p_vaddr is the
    // absolute load address). load_elf_image is shared with the interpreter
    // path below; it returns the phdr VA, brk end, and entry for this image.
    LoadedImage  main_img{};
    ExecveResult load_res =
        load_elf_image(*task->addr_space, inode, ehdr, phdrs, phnum, /*base=*/0, main_img);
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
        auto pread = inode->ops->read(inode, phdrs[i].p_offset, interp_path, plen);
        if (!pread.ok() || pread.value() < static_cast<int64_t>(plen)) {
            cinux::lib::kprintf("[EXECVE] PT_INTERP path read failed\n");
            delete[] phdrs;
            return ExecveResult::ReadFailed;
        }
        interp_path[plen] = '\0';  // PT_INTERP usually includes the NUL; force it.
        has_interp        = true;
        break;
    }

    delete[] phdrs;

    if (!main_img.has_load) {
        cinux::lib::kprintf("[EXECVE] no PT_LOAD segments found\n");
        return ExecveResult::NoLoadSegments;
    }

    // F2-M3: initialise the user heap.  brk starts at the page-aligned end of
    // the ELF image; the Heap VMA spans [brk_initial, USER_BRK_MAX) so demand
    // paging services heap growth without further bookkeeping.
    //
    // F9 batch 8 (ASLR): add a page-aligned random gap above the image so the
    // heap start moves per-exec. Clamped to stay under USER_BRK_MAX with real
    // heap room left (our ~4 MB image never trips the clamp).
    uint64_t brk_start = (main_img.max_seg_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint64_t brk_gap   = cinux::lib::aslr_brk_offset();
    if (brk_start + brk_gap >= cinux::arch::USER_BRK_MAX) {
        brk_gap = 0;
    }
    task->brk_initial = brk_start + brk_gap;
    task->brk_current = task->brk_initial;
    task->brk_max     = cinux::arch::USER_BRK_MAX;
    constexpr cinux::mm::VmaFlags kHeapVma =
        cinux::mm::VmaFlags::Read | cinux::mm::VmaFlags::Write | cinux::mm::VmaFlags::Heap;
    if (!task->addr_space->vmas()
             .insert(task->brk_initial, cinux::arch::USER_BRK_MAX, kHeapVma)
             .ok()) {
        cinux::lib::kprintf("[EXECVE] heap VMA record failed\n");
        return ExecveResult::MapFailed;
    }

    // F10-M2: load the dynamic interpreter when PT_INTERP was present. The
    // interpreter (ld-musl / ld-linux) is mapped at USER_INTERP_BASE and we
    // enter IT -- after relocating the main program it jumps to AT_ENTRY (the
    // main entry). A static executable (no PT_INTERP) enters the main entry
    // directly, unchanged from before.
    uint64_t interp_base = 0;
    uint64_t entry_va    = ehdr->e_entry;  // static default
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
            cinux::mm::g_pmm.free_page(sigret_phys);
            return ExecveResult::MapFailed;
        }
    }

    if (aux_out != nullptr) {
        aux_out->at_phdr    = main_img.phdr_va;
        aux_out->at_phnum   = ehdr->e_phnum;
        aux_out->at_phent   = sizeof(elf::Elf64_Phdr);
        aux_out->at_entry   = ehdr->e_entry;  // ldso jumps here after relocating
        aux_out->at_base    = interp_base;    // interpreter load base (0 if static)
        aux_out->has_interp = has_interp;
    }

    cinux::lib::kprintf("[EXECVE] loaded %s entry=%p%s pid=%d\n", path,
                        reinterpret_cast<void*>(entry_va), has_interp ? " (dynamic)" : "",
                        task->pid);

    return ExecveResult::Ok;
}

}  // namespace cinux::proc
