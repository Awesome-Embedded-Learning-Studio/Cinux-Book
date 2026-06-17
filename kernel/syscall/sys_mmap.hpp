/**
 * @file kernel/syscall/sys_mmap.hpp
 * @brief mmap/munmap/mprotect syscall declarations (F2-M2)
 *
 * Linux x86_64 memory-mapping syscalls built on the M1 VMA store.  mmap is
 * lazy: it records a VMA but maps no physical pages -- the first access faults
 * and is served by demand paging.  This header owns the POSIX prot/flags
 * constants shared with user space.
 *
 * Namespace: cinux::syscall
 */

#pragma once

#include <stdint.h>

#include "kernel/arch/x86_64/syscall.hpp"

namespace cinux::syscall {

// ============================================================
// POSIX mmap prot (Linux x86_64 ABI values)
// ============================================================

constexpr uint64_t PROT_NONE  = 0x0;
constexpr uint64_t PROT_READ  = 0x1;
constexpr uint64_t PROT_WRITE = 0x2;
constexpr uint64_t PROT_EXEC  = 0x4;

// ============================================================
// POSIX mmap flags
// ============================================================

constexpr uint64_t MAP_SHARED    = 0x01;
constexpr uint64_t MAP_PRIVATE   = 0x02;
constexpr uint64_t MAP_FIXED     = 0x10;
constexpr uint64_t MAP_ANONYMOUS = 0x20;

/**
 * @brief Map virtual memory (Linux syscall 9)
 *
 * Anonymous mappings only in M2 batch 1 (file mappings arrive in batch 4).
 * Lazy: registers a VMA but defers physical allocation to the page-fault path.
 *
 * @return Mapped base address (>=0), or -errno on failure.
 */
int64_t sys_mmap(uint64_t addr, uint64_t length, uint64_t prot, uint64_t flags, uint64_t fd,
                 uint64_t offset);

/**
 * @brief Unmap virtual memory (Linux syscall 11)
 *
 * Removes VMAs covering [addr, addr+length) and frees any physical pages that
 * were demand-paged into the range, dropping their PTEs.  Partial overlap
 * splits a VMA.  Unmapping a region with no mapping is not an error.
 *
 * @return 0 on success, or -errno on failure.
 */
int64_t sys_munmap(uint64_t addr, uint64_t length, uint64_t, uint64_t, uint64_t, uint64_t);

/**
 * @brief Set protection on a region (Linux syscall 10)
 *
 * Updates the access flags (PROT_READ/WRITE/EXEC) of every VMA covering
 * [addr, addr+length), splitting as needed, and re-issues the PTE permissions
 * for any already-mapped pages.  Other VMA attributes (Anonymous/Shared) are
 * preserved.
 *
 * @return 0 on success, or -errno on failure.
 */
int64_t sys_mprotect(uint64_t addr, uint64_t length, uint64_t prot, uint64_t, uint64_t, uint64_t);

}  // namespace cinux::syscall
