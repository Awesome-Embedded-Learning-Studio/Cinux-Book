/**
 * @file kernel/proc/elf_load.hpp
 * @brief Shared ELF PT_LOAD segment mapper (F10-M2)
 *
 * Extracted from execve.cpp so the main program and the dynamic interpreter
 * share one segment-mapping path. Maps a validated ELF's PT_LOAD segments from
 * a VFS inode into an address space at a given base:
 *
 *   - ET_EXEC main program (non-PIE): base = 0; p_vaddr is the absolute VA.
 *   - ET_DYN interpreter (ld-musl / ld-linux): base = USER_INTERP_BASE;
 *     each segment VA = base + p_vaddr.
 *
 * The caller still owns header read + validation + phdr read; this helper does
 * only the per-page alloc/zero/read/map/VMA-record loop. clear_user_mappings()
 * and the brk/sigreturn setup stay in execve.cpp (main-program-specific). The
 * interpreter does its own GOT/PLT relocation and DT_NEEDED loading in user
 * space -- the kernel never parses PT_DYNAMIC.
 */

#pragma once

#include <cstdint>

#include "kernel/proc/elf_types.hpp"
#include "kernel/proc/execve.hpp"  // ExecveResult

namespace cinux::fs {
struct Inode;
}

namespace cinux::mm {
class AddressSpace;
}

namespace cinux::proc {

/// Outcome of mapping one ELF image: the values the caller needs for auxv/brk.
struct LoadedImage {
    uint64_t entry;        ///< e_entry of this image (caller adds base for ET_DYN)
    uint64_t phdr_va;      ///< User VA of this image's phdr table (base applied)
    uint64_t max_seg_end;  ///< Highest PT_LOAD end VA (main uses it for brk_initial)
    bool     has_load;     ///< False if the image had no PT_LOAD (caller rejects)
};

/**
 * @brief Map all PT_LOAD segments of a validated ELF into @p space at @p base.
 *
 * Reads each PT_LOAD segment page-by-page from @p inode, zero-fills the page,
 * copies the on-disk bytes (p_filesz) into place, maps it, and records a VMA
 * with the segment's permissions so demand-page/mprotect see it.
 *
 * Does NOT clear existing mappings (the caller clears once before loading the
 * main image; the interpreter is then mapped into the already-cleared space).
 *
 * @param space  Target address space.
 * @param inode  VFS inode backing the ELF file (read via inode->ops->read).
 * @param ehdr   Validated ELF header (e_phoff used to locate this image's phdrs).
 * @param phdrs  Program-header buffer (caller-owned).
 * @param phnum  Number of program headers.
 * @param base   Load base (0 for ET_EXEC, USER_INTERP_BASE for the interpreter).
 * @param out    Filled on success.
 * @return ExecveResult::Ok on success; MapFailed/ReadFailed otherwise (out untouched on error).
 */
ExecveResult load_elf_image(cinux::mm::AddressSpace& space, cinux::fs::Inode* inode,
                            const elf::Elf64_Ehdr* ehdr, const elf::Elf64_Phdr* phdrs,
                            uint16_t phnum, uint64_t base, LoadedImage& out);

/**
 * @brief Resolve and load the dynamic interpreter (PT_INTERP target).
 *
 * Given the NUL-terminated interpreter path from the main program's PT_INTERP
 * segment (e.g. "/lib/ld-musl-x86_64.so.1"), resolve it on the VFS, validate
 * its ELF header (ET_DYN), and map its PT_LOAD segments at USER_INTERP_BASE.
 * On success @p out_base is the interpreter load base (USER_INTERP_BASE) and
 * @p out_entry is the absolute user entry VA the kernel should jump to
 * (USER_INTERP_BASE + interp.e_entry). The interpreter then relocates the main
 * program and jumps to AT_ENTRY.
 *
 * @return ExecveResult::Ok on success; otherwise FileNotFound, FileNotRegular,
 *         ReadFailed, BadElfHeaders, MapFailed, or NoLoadSegments.
 */
ExecveResult load_interpreter(cinux::mm::AddressSpace& space, const char* path, uint64_t& out_base,
                              uint64_t& out_entry);

}  // namespace cinux::proc
