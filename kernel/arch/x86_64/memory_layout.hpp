#pragma once

#include <stdint.h>

namespace cinux::arch {

// Higher-half direct mapping: physaddr + KERNEL_VMA = canonical virtual address.
// Must match the offset used in linker.ld.
//
// NOTE: KERNEL_VMA doubles as the kernel IMAGE base (the kernel is linked and
// mapped at KERNEL_VMA + KERNEL_LMA).  It is NOT a usable direct map for all
// RAM: the higher-half window above 0xFFFFFFFF80000000 is only 2 GB, and the
// bootloader maps just the first 1 GB there.  Use DIRECT_MAP_BASE for arbitrary
// phys->virt (phys_to_virt); KERNEL_VMA stays for kernel-image-relative math.
constexpr uint64_t KERNEL_VMA = 0xFFFFFFFF80000000ULL;

// Direct map: a dedicated 512 GB window (PML4[272]) where the loader identity-
// maps ALL physical RAM with 1 GB huge pages.  Unlike KERNEL_VMA this window is
// large enough for any RAM size and is fully identity-mapped at load time, so
// phys_to_virt(phys) = DIRECT_MAP_BASE + phys works for every page the PMM can
// return.  Must match the window mapped by the mini loader (direct_map_up_to).
constexpr uint64_t DIRECT_MAP_BASE = 0xFFFF880000000000ULL;

// ============================================================
// Kernel virtual memory layout (0xFFFF8000_00000000+)
// ============================================================
// Regions are defined as (base, size) pairs.  Each subsequent
// region starts at the previous region's base + size.
// To add a new region, insert it here and bump the ones below.

constexpr uint64_t KMEM_BASE = 0xFFFF800000000000ULL;

// Slab: small-object slab allocator pages (F2-M7b).  Intrusive free-list links
// are written INSIDE these pages, so they MUST be ordinary 4 KB mappings --
// never the direct-map huge window (sub-page writes there are not reliably
// read back under WSL2 nested-KVM; see GOTCHA #13/#14).  32 MB is ample: slab
// pages are few (one per active size class, cached for object reuse).
constexpr uint64_t KMEM_SLAB_SIZE = 0x2000000ULL;  // 32 MB
constexpr uint64_t KMEM_SLAB_BASE = KMEM_BASE;

// Heap: kernel heap allocator (deleted in F2-M7b batch 2 once kmalloc replaces
// it; the region is kept here until that swap lands).
constexpr uint64_t KMEM_HEAP_SIZE = 0x8000000ULL;  // 128 MB
constexpr uint64_t KMEM_HEAP_BASE = KMEM_SLAB_BASE + KMEM_SLAB_SIZE;

// MMIO: memory-mapped I/O (AHCI BAR5, etc.)
constexpr uint64_t KMEM_MMIO_SIZE = 0x200000ULL;  // 2 MB
constexpr uint64_t KMEM_MMIO_BASE = KMEM_HEAP_BASE + KMEM_HEAP_SIZE;

// Framebuffer: linear framebuffer MMIO (2 MB-aligned for huge page mapping)
constexpr uint64_t KMEM_FB_SIZE = 0x1000000ULL;  // 16 MB
constexpr uint64_t KMEM_FB_BASE = KMEM_MMIO_BASE + KMEM_MMIO_SIZE;

// Stacks: per-task kernel stacks (allocated upward)
constexpr uint64_t KMEM_STACK_BASE = KMEM_FB_BASE + KMEM_FB_SIZE;

// DMA: ad-hoc DMA buffers (early-boot sector reads, etc.). Static window for
// code that runs before the DmaPool is usable (mini loader, main.cpp). The
// DmaPool (F1-M3) does NOT consume this region: it reuses the higher-half
// direct map (phys + KERNEL_VMA), so pool allocations need no reserved range.
constexpr uint64_t KMEM_DMA_SIZE = 0x100000ULL;  // 1 MB
constexpr uint64_t KMEM_DMA_BASE = KMEM_STACK_BASE + 0x100000ULL;

// ext2 DMA: ext2 filesystem block cache / DMA buffers
constexpr uint64_t KMEM_EXT2_DMA_SIZE = 0x100000ULL;  // 1 MB
constexpr uint64_t KMEM_EXT2_DMA_BASE = KMEM_DMA_BASE + KMEM_DMA_SIZE;

// ============================================================
// User-space virtual memory layout (lower half)
// ============================================================
// The ELF image loads at USER_ENTRY_BASE (see usermode.hpp); the heap, mmap
// region and stack all live below USER_STACK_TOP.  Cinux's user space tops out
// near 32 GB (USER_STACK_TOP = 0x7FFFFF000), far below the canonical 128 TB, so
// the mmap window is sized to leave a guard gap below the stack.  These are
// consumed by mmap (F2-M2) and brk (F2-M3); F2-M1 only defines them.

/// Heap start, just past the ELF image (USER_ENTRY_BASE = 0x400000).
constexpr uint64_t USER_BRK_BASE  = 0x600000ULL;  // 6 MB
/// Heap ceiling for brk growth.  F4-B0: 240 MB -- the gap below the interpreter
/// base (USER_INTERP_BASE = 256 MB) so brk cannot collide with the ldso image.
/// Was 64 MB (too tight for glibc malloc arenas under cc1/as/ld); a true dynamic
/// ceiling / interpreter ASLR is a follow-up noted under the GCC self-host line.
constexpr uint64_t USER_BRK_MAX   = 0xF000000ULL;  // 240 MB
/// Start of the mmap region (above the heap ceiling).
constexpr uint64_t USER_MMAP_BASE = 0x100000000ULL;  // 4 GB
/// End of the mmap region -- 8 GB below USER_STACK_TOP as a guard gap.
constexpr uint64_t USER_MMAP_END  = 0x600000000ULL;  // 24 GB

/// Load base for the dynamic interpreter (ld-musl / ld-linux), F10-M2. The
/// interpreter is ET_DYN (position-independent), so the kernel maps its
/// PT_LOAD segments at USER_INTERP_BASE + p_vaddr and reports this base via
/// AT_BASE. Placed in the unused gap between the heap ceiling (USER_BRK_MAX =
/// 240 MB) and the mmap region (USER_MMAP_BASE = 4 GB) so it cannot collide
/// with either the brk heap or user mmap allocations. The interpreter finds
/// itself via __ehdr_start, so any base works; this is fixed for determinism
/// (ASLR of the interpreter base is a follow-up).
constexpr uint64_t USER_INTERP_BASE = 0x10000000ULL;  // 256 MB

/// Load base for a PIE main executable (ET_DYN). The main-program counterpart
/// of USER_INTERP_BASE: a PIE binary's p_vaddr is base-relative, so the kernel
/// maps PT_LOAD at USER_EXEC_BASE + aslr_exec_base_offset() + p_vaddr. Placed
/// above the interpreter image (USER_INTERP_BASE = 256 MB + ldso size) and well
/// below the mmap region (USER_MMAP_BASE = 4 GB); the 0 .. 16 MB ASLR window
/// [0x20000000, 0x21000000) collides with neither. ET_EXEC mains are absolute
/// and bypass this (the loader passes base 0).
constexpr uint64_t USER_EXEC_BASE = 0x20000000ULL;  // 512 MB

}  // namespace cinux::arch
