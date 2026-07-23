/**
 * @file kernel/proc/elf_load.cpp
 * @brief Shared ELF PT_LOAD segment mapper implementation (F10-M2)
 *
 * The per-page alloc/zero/read/map/VMA-record loop, parameterised by a load
 * base so it serves both the ET_EXEC main program (base 0) and the ET_DYN
 * interpreter (base USER_INTERP_BASE). See elf_load.hpp.
 */

#include "kernel/proc/elf_load.hpp"

#include <stddef.h>
#include <stdint.h>

#include <new>

#include "kernel/arch/x86_64/memory_layout.hpp"  // USER_INTERP_BASE
#include "kernel/arch/x86_64/paging.hpp"
#include "kernel/arch/x86_64/paging_config.hpp"
#include "kernel/fs/inode.hpp"
#include "kernel/fs/vfs_mount.hpp"  // vfs_resolve
#include "kernel/lib/kprintf.hpp"
#include "kernel/mm/address_space.hpp"
#include "kernel/mm/pmm.hpp"

namespace cinux::proc {

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

        uint64_t page_flags = FLAG_PRESENT | FLAG_USER;
        if (phdr.p_flags & elf::PF_W) {
            page_flags |= FLAG_WRITABLE;
        }
        if (!(phdr.p_flags & elf::PF_X)) {
            page_flags |= FLAG_NX;
        }

        for (uint64_t vaddr = seg_start; vaddr < seg_end; vaddr += PAGE_SIZE) {
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

            // The head page of a segment may precede p_vaddr; data_vaddr is the
            // first real byte. seg_offset is the byte offset into the segment
            // (identical in file and memory, base-independent for the file read).
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
                    cinux::mm::g_pmm.free_page(phys);
                    return ExecveResult::ReadFailed;
                }
            }

            if (!space.map(vaddr, phys, page_flags)) {
                cinux::lib::kprintf("[ELF] map failed at vaddr=%p\n",
                                    reinterpret_cast<void*>(vaddr));
                cinux::mm::g_pmm.free_page(phys);
                return ExecveResult::MapFailed;
            }
        }

        // Record the segment VMA so the page-fault handler and mprotect honour
        // its permissions (the interpreter mprotects RELRO ranges read-only).
        cinux::mm::VmaFlags seg_vma = cinux::mm::VmaFlags::Read;
        if (phdr.p_flags & elf::PF_W) {
            seg_vma |= cinux::mm::VmaFlags::Write;
        }
        if (phdr.p_flags & elf::PF_X) {
            seg_vma |= cinux::mm::VmaFlags::Exec;
        }
        if (!space.vmas().insert(seg_start, seg_end, seg_vma).ok()) {
            cinux::lib::kprintf("[ELF] VMA record failed for %p-%p\n",
                                reinterpret_cast<void*>(seg_start),
                                reinterpret_cast<void*>(seg_end));
            return ExecveResult::MapFailed;
        }
    }

    return ExecveResult::Ok;
}

ExecveResult load_interpreter(cinux::mm::AddressSpace& space, const char* path, uint64_t& out_base,
                              uint64_t& out_entry) {
    out_base  = 0;
    out_entry = 0;

    // Resolve the interpreter path on the VFS (e.g. /lib/ld-musl-x86_64.so.1).
    const char* rel = nullptr;
    auto*       fs  = cinux::fs::vfs_resolve(path, &rel);
    if (fs == nullptr) {
        cinux::lib::kprintf("[EXECVE] interp not found: %s\n", path);
        return ExecveResult::FileNotFound;
    }
    auto lookup = fs->lookup(rel);
    if (!lookup.ok()) {
        cinux::lib::kprintf("[EXECVE] interp inode not found: %s\n", rel);
        return ExecveResult::FileNotFound;
    }
    auto* inode = lookup.value();
    if (inode->type != cinux::fs::InodeType::Regular) {
        cinux::lib::kprintf("[EXECVE] interp not a regular file: %s\n", path);
        return ExecveResult::FileNotRegular;
    }

    // Read + validate the interpreter ELF header (it is ET_DYN, which
    // validate_elf_header accepts as of F10-M2).
    elf::Elf64_Ehdr ehdr_buf;
    auto            nread = inode->ops->read(inode, 0, &ehdr_buf, sizeof(elf::Elf64_Ehdr));
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
    auto pread = inode->ops->read(inode, ehdr->e_phoff, phdrs, phdr_bytes);
    if (!pread.ok() || pread.value() < static_cast<int64_t>(phdr_bytes)) {
        cinux::lib::kprintf("[EXECVE] interp phdr read failed\n");
        delete[] phdrs;
        return ExecveResult::ReadFailed;
    }

    // Map the interpreter at USER_INTERP_BASE (ET_DYN: segments are base-relative).
    LoadedImage  img{};
    ExecveResult load_res =
        load_elf_image(space, inode, ehdr, phdrs, phnum, cinux::arch::USER_INTERP_BASE, img);
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
    cinux::lib::kprintf("[EXECVE] loaded interp %s base=%p entry=%p\n", path,
                        reinterpret_cast<void*>(out_base), reinterpret_cast<void*>(out_entry));
    return ExecveResult::Ok;
}

}  // namespace cinux::proc
