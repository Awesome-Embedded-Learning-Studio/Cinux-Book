/**
 * @file kernel/proc/elf_load.cpp
 * @brief Shared ELF PT_LOAD segment mapper implementation (F10-M2)
 *
 * Each PT_LOAD segment gets a file-backed VMA for its whole file pages (lazy
 * demand-paged by handle_pf via PageCache::get_page, B2 -- the bulk of large
 * ELFs like cc1's .text); the single page straddling p_filesz (plus any pure
 * BSS pages after it) is eager-mapped with a precise zero-fill. This mirrors
 * Linux load_elf_binary: elf_map() for whole file pages, padzero() for the
 * straddle page's BSS tail, set_brk() for the pure-BSS pages. Parameterised by
 * a load base so it serves both the ET_EXEC main program (base 0) and the
 * ET_DYN interpreter (base USER_INTERP_BASE). See elf_load.hpp.
 */

#include "kernel/proc/elf_load.hpp"

#include <stddef.h>
#include <stdint.h>

#include <new>

#include "kernel/arch/x86_64/memory_layout.hpp"  // USER_INTERP_BASE
#include "kernel/arch/x86_64/paging.hpp"
#include "kernel/arch/x86_64/paging_config.hpp"
#include "kernel/fs/file.hpp"         // inode_ref (B2 lazy PT_LOAD VMA backing)
#include "kernel/fs/inode.hpp"
#include "kernel/fs/inode_ref.hpp"     // InodeRef: RAII over the lookup ref
#include "kernel/fs/vfs_lookup.hpp"  // F-USABILITY batch 1c: vfs_lookup (interp follow)
#include "kernel/lib/kprintf.hpp"
#include "kernel/mm/address_space.hpp"  // VmaFlags / VMA / vmas() (B2 lazy PT_LOAD)
#include "kernel/mm/pmm.hpp"             // g_pmm (eager path for BSS segments)

namespace cinux::proc {

namespace {

// DEBT-020: validate a PT_LOAD segment's fields before any arithmetic, so a
// malformed/hostile ELF can't wrap p_vaddr+p_memsz into a tiny/wrong seg_end
// and trick us into mapping the wrong VMA range. GCC has no -fsanitize for
// unsigned overflow (Clang-only, per F-VERIFY M0-2), so use __builtin_*_overflow.
ExecveResult validate_load_segment(const elf::Elf64_Phdr& phdr, uint64_t base) {
    // ELF spec: a segment's file image can't exceed its memory image.
    if (phdr.p_filesz > phdr.p_memsz) {
        return ExecveResult::BadElfHeaders;
    }
    uint64_t seg_vaddr = 0, seg_memsz_end = 0;
    [[maybe_unused]] uint64_t file_off_end = 0;  // p_offset + p_filesz
    // base + p_vaddr, seg_vaddr + p_memsz, p_offset + p_filesz must not wrap.
    if (__builtin_add_overflow(base, phdr.p_vaddr, &seg_vaddr) ||
        __builtin_add_overflow(seg_vaddr, phdr.p_memsz, &seg_memsz_end) ||
        __builtin_add_overflow(phdr.p_offset, phdr.p_filesz, &file_off_end)) {
        return ExecveResult::BadElfHeaders;
    }
    // Stay inside canonical user VA (below the 48-bit canonical hole at
    // 0x800000000000); a segment mapping into non-canonical/kernel space is
    // corrupt or hostile.
    constexpr uint64_t kUserVaTop = 0x800000000000ULL;
    if (seg_vaddr >= kUserVaTop || seg_memsz_end > kUserVaTop) {
        return ExecveResult::BadElfHeaders;
    }
    return ExecveResult::Ok;
}

}  // namespace

ExecveResult load_elf_image(cinux::mm::AddressSpace& space, cinux::fs::Inode* inode,
                            const elf::Elf64_Ehdr* ehdr, const elf::Elf64_Phdr* phdrs,
                            uint16_t phnum, uint64_t base, LoadedImage& out) {
    using namespace cinux::arch;

    out.has_load    = false;
    out.entry       = ehdr->e_entry;
    out.phdr_va     = 0;
    out.max_seg_end = 0;

    for (uint16_t i = 0; i < phnum; i++) {
        const auto& phdr = phdrs[i];
        if (phdr.p_type != elf::PT_LOAD) {
            continue;
        }

        out.has_load = true;

        // DEBT-020: reject segments whose p_vaddr/p_memsz/p_filesz would wrap
        // or escape canonical user VA before we compute seg_end / map VMAs.
        if (ExecveResult vr = validate_load_segment(phdr, base); vr != ExecveResult::Ok) {
            cinux::lib::kprintf(
                "[ELF] rejecting bad PT_LOAD seg %u: vaddr=0x%lx memsz=0x%lx filesz=0x%lx\n",
                i, static_cast<unsigned long>(phdr.p_vaddr),
                static_cast<unsigned long>(phdr.p_memsz), static_cast<unsigned long>(phdr.p_filesz));
            return vr;
        }

        // AT_PHDR for THIS image: the phdr table lives inside the first PT_LOAD
        // that covers e_phoff. User VA = base + p_vaddr + (e_phoff - p_offset).
        // The main program's value is what musl reads via AT_PHDR; for the
        // interpreter it is unused (ldso finds itself via __ehdr_start).
        if (phdr.p_offset <= ehdr->e_phoff && ehdr->e_phoff < phdr.p_offset + phdr.p_filesz) {
            out.phdr_va = base + phdr.p_vaddr + (ehdr->e_phoff - phdr.p_offset);
        }

        uint64_t seg_vaddr = base + phdr.p_vaddr;
        uint64_t seg_start = seg_vaddr & ~(PAGE_SIZE - 1);
        uint64_t seg_end   = (seg_vaddr + phdr.p_memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        if (seg_end > out.max_seg_end) {
            out.max_seg_end = seg_end;
        }

        // ---- B2 (Linux load_elf_binary style). Each PT_LOAD segment is split
        // three ways:
        //   (a) whole file pages  -> file-backed VMA, lazy demand-paged by
        //                            handle_pf via PageCache::get_page
        //                           (= Linux elf_map; the bulk of large ELFs like
        //                            cc1's 47 MB .text, previously eager-read at
        //                            ~22 s of AHCI I/O per the B1 stats curve)
        //   (b) straddle page      -> eager: holds the last p_filesz tail bytes
        //                            plus same-page BSS zero-fill
        //                           (= Linux padzero + one CoW write)
        //   (c) pure-BSS pages     -> eager: anonymous zero pages
        //                           (= Linux set_brk)
        // The straddle page MUST be eager because lazy get_page reads the file
        // past p_filesz into the section headers and would pollute same-page BSS.
        cinux::mm::VmaFlags seg_vma = cinux::mm::VmaFlags::Read;
        if (phdr.p_flags & elf::PF_W) {
            seg_vma |= cinux::mm::VmaFlags::Write;
        }
        if (phdr.p_flags & elf::PF_X) {
            seg_vma |= cinux::mm::VmaFlags::Exec;
        }

        // seg_start maps to file offset p_offset - (seg_vaddr - seg_start); ELF
        // requires p_offset ≡ p_vaddr (mod PAGE_SIZE), so this stays page-aligned.
        const uint64_t file_off_base  = phdr.p_offset - (seg_vaddr - seg_start);
        // Last VA covered by a WHOLE file page = p_filesz rounded DOWN to a page.
        const uint64_t file_pages_end = (seg_vaddr + phdr.p_filesz) & ~(PAGE_SIZE - 1);

        // (a) Whole-file pages [seg_start, file_pages_end): lazy file-backed VMA.
        if (phdr.p_filesz > 0 && file_pages_end > seg_start) {
            auto ir = space.vmas().insert(seg_start, file_pages_end, seg_vma);
            if (!ir.ok()) {
                cinux::lib::kprintf("[ELF] VMA record failed for %p-%p\n",
                                    reinterpret_cast<void*>(seg_start),
                                    reinterpret_cast<void*>(file_pages_end));
                return ExecveResult::MapFailed;
            }
            if (cinux::mm::VMA* v = space.vmas().find(seg_start)) {
                v->backing     = inode;
                v->file_offset = file_off_base;
                cinux::fs::inode_ref(inode);  // VMA holds a ref; release_backing unrefs on clear/split
            }
        }

        // (b)+(c) Straddle page + pure-BSS pages [file_pages_end, seg_end): eager
        // per-page alloc+zero+read+map. The straddle page reads the file tail
        // (seg_offset < p_filesz); pure-BSS pages read nothing and stay zero.
        if (seg_end > file_pages_end) {
            uint64_t page_flags = FLAG_PRESENT | FLAG_USER;
            if (phdr.p_flags & elf::PF_W) {
                page_flags |= FLAG_WRITABLE;
            }
            if (!(phdr.p_flags & elf::PF_X)) {
                page_flags |= FLAG_NX;
            }
            for (uint64_t vaddr = file_pages_end; vaddr < seg_end; vaddr += PAGE_SIZE) {
                uint64_t phys = cinux::mm::g_pmm.alloc_page();
                if (phys == 0) {
                    cinux::lib::kprintf("[ELF] page alloc failed at vaddr=%p\n",
                                        reinterpret_cast<void*>(vaddr));
                    return ExecveResult::MapFailed;
                }
                auto* dst = reinterpret_cast<uint8_t*>(phys + cinux::arch::DIRECT_MAP_BASE);
                for (uint64_t b = 0; b < PAGE_SIZE; b++) {
                    dst[b] = 0;
                }
                uint64_t data_vaddr  = (vaddr < seg_vaddr) ? seg_vaddr : vaddr;
                uint64_t in_page_off = data_vaddr - vaddr;
                uint64_t seg_offset  = data_vaddr - seg_vaddr;
                if (seg_offset < phdr.p_filesz) {
                    uint64_t copy_len = phdr.p_filesz - seg_offset;
                    uint64_t avail    = PAGE_SIZE - in_page_off;
                    if (copy_len > avail) {
                        copy_len = avail;
                    }
                    auto bread = inode->ops->read(inode, phdr.p_offset + seg_offset, dst + in_page_off,
                                                  copy_len);
                    if (!bread.ok() || bread.value() < static_cast<int64_t>(copy_len)) {
                        cinux::lib::kprintf("[ELF] segment read failed at offset %lu\n",
                                            static_cast<unsigned long>(phdr.p_offset + seg_offset));
                        cinux::mm::g_pmm.refcount_dec_and_test(phys);
                        return ExecveResult::ReadFailed;
                    }
                }
                if (!space.map(vaddr, phys, page_flags)) {
                    cinux::lib::kprintf("[ELF] map failed at vaddr=%p\n",
                                        reinterpret_cast<void*>(vaddr));
                    cinux::mm::g_pmm.refcount_dec_and_test(phys);
                    return ExecveResult::MapFailed;
                }
                cinux::mm::g_pmm.pte_count_inc(phys);
            }
            // Eager-mapped tail is private memory (no backing file). Tag it
            // Anonymous so vmas().insert does NOT merge it with the file-backed
            // VMA above -- a merge drops the file-backed VMA's backing inode ref
            // (release_backing), turning lazy .text pages into anonymous zero
            // pages and crashing on execute (0x00 0x00 = add %al,(%rax) into
            // NULL). Different flags => no merge; the eager tail stays private.
            if (!space.vmas().insert(file_pages_end, seg_end,
                                     seg_vma | cinux::mm::VmaFlags::Anonymous)
                     .ok()) {
                cinux::lib::kprintf("[ELF] tail VMA record failed for %p-%p\n",
                                    reinterpret_cast<void*>(file_pages_end),
                                    reinterpret_cast<void*>(seg_end));
                return ExecveResult::MapFailed;
            }
        }
    }

    return ExecveResult::Ok;
}

ExecveResult load_interpreter(cinux::mm::AddressSpace& space, const char* path, uint64_t& out_base,
                              uint64_t& out_entry) {
    out_base  = 0;
    out_entry = 0;

    // F-USABILITY batch 1c: vfs_lookup follows the interp symlink (e.g.
    // /lib/ld-musl-x86_64.so.1 -> libc.so) just like the main executable.
    // PT_INTERP is absolute, so cwd="/" is sufficient.
    auto lr = cinux::fs::vfs_lookup(path, static_cast<uint32_t>(cinux::fs::LookupFlag::Follow), "/");
    if (!lr.ok()) {
        cinux::lib::kprintf("[EXECVE] interp not found: %s\n", path);
        return ExecveResult::FileNotFound;
    }
    cinux::fs::InodeRef inode(lr.value().target);  // RAII: lookup ref dropped at every return
    if (inode->type != cinux::fs::InodeType::Regular) {
        cinux::lib::kprintf("[EXECVE] interp not a regular file: %s\n", path);
        return ExecveResult::FileNotRegular;
    }

    // Read + validate the interpreter ELF header (it is ET_DYN, which
    // validate_elf_header accepts as of F10-M2).
    elf::Elf64_Ehdr ehdr_buf;
    auto            nread = inode->ops->read(inode.get(), 0, &ehdr_buf, sizeof(elf::Elf64_Ehdr));
    if (!nread.ok() || nread.value() < static_cast<int64_t>(sizeof(elf::Elf64_Ehdr))) {
        cinux::lib::kprintf("[EXECVE] interp header read failed\n");
        return ExecveResult::ReadFailed;
    }
    auto* ehdr = &ehdr_buf;
    auto  vr   = elf::validate_elf_header(ehdr, inode->size);
    if (vr != elf::ElfValidateResult::Ok) {
        cinux::lib::kprintf("[EXECVE] interp ELF validation failed: %d\n", static_cast<int>(vr));
        return ExecveResult::BadElfHeaders;
    }

    // Read the interpreter's program headers.
    uint16_t phnum      = ehdr->e_phnum;
    uint64_t phdr_bytes = static_cast<uint64_t>(phnum) * sizeof(elf::Elf64_Phdr);
    auto*    phdrs      = new (std::align_val_t{alignof(elf::Elf64_Phdr)}) elf::Elf64_Phdr[phnum];
    if (phdrs == nullptr) {
        return ExecveResult::MapFailed;
    }
    auto pread = inode->ops->read(inode.get(), ehdr->e_phoff, phdrs, phdr_bytes);
    if (!pread.ok() || pread.value() < static_cast<int64_t>(phdr_bytes)) {
        cinux::lib::kprintf("[EXECVE] interp phdr read failed\n");
        delete[] phdrs;
        return ExecveResult::ReadFailed;
    }

    // Map the interpreter at USER_INTERP_BASE (ET_DYN: segments are base-relative).
    LoadedImage  img{};
    ExecveResult load_res =
        load_elf_image(space, inode.get(), ehdr, phdrs, phnum, cinux::arch::USER_INTERP_BASE, img);
    delete[] phdrs;
    if (load_res != ExecveResult::Ok) {
        return load_res;
    }
    if (!img.has_load) {
        cinux::lib::kprintf("[EXECVE] interp has no PT_LOAD segments\n");
        return ExecveResult::NoLoadSegments;
    }

    out_base  = cinux::arch::USER_INTERP_BASE;
    out_entry = cinux::arch::USER_INTERP_BASE + ehdr->e_entry;  // ET_DYN: entry is base-relative
    return ExecveResult::Ok;
}

}  // namespace cinux::proc
