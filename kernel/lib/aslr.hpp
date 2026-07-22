/**
 * @file kernel/lib/aslr.hpp
 * @brief ASLR offset helpers (F9 batch 8 / M2)
 *
 * Page-aligned random offsets for the three kernel-chosen user-space
 * addresses -- the user stack top, the mmap search start, and the brk heap
 * start. Each offset is page-aligned so existing alignment invariants are
 * preserved (notably the x86_64 ABI RSP rule: USER_STACK_TOP - offset stays
 * 16-byte aligned, so subtracting USER_ABI_RSP_OFFSET still yields %16==8).
 *
 * What is NOT randomized here: the ELF text/data base. Cinux's user program
 * is non-PIE (-fno-pie -no-pie -mcmodel=small -static): global and string
 * addresses are baked in as 32-bit absolute immediates (verified: 0 RIP-
 * relative references, 99 absolute), so relocating the image would corrupt
 * every absolute reference. Text-segment ASLR needs PIE + kernel-side
 * dynamic-relocation processing (R_X86_64_RELATIVE) and is tracked as a
 * separate follow-up.
 *
 * Honest scope: these feed the boot-seeded KRandom (F9 batch 7), good enough
 * to defeat a naive address guess, not a hardened CSPRNG.
 *
 * Namespace: cinux::lib
 */

#pragma once

#include <stdint.h>

#include "kernel/lib/random.hpp"

namespace cinux::lib {

/// Stack-top randomization: 0 .. ~8 MiB, page-aligned (~11 bits of page
/// entropy). Subtracted from USER_STACK_TOP; stays clear of the 1 MiB
/// demand-growth region below it.
inline uint64_t aslr_stack_offset() {
    return g_random.next64() & 0x7FF000ULL;  // 0 .. 8 MiB - 4K
}

/// mmap search-start jitter: 0 .. ~1 GiB, page-aligned. The mmap window
/// itself (USER_MMAP_BASE .. USER_MMAP_END = 4..24 GiB) is unchanged; only
/// the first-fit hint moves, so each process's mappings land unpredictably.
/// MAP_FIXED ignores this and honours the caller's address.
inline uint64_t aslr_mmap_offset() {
    return g_random.next64() & 0x3FFFF000ULL;  // 0 .. ~1 GiB - 4K
}

/// brk heap-start gap: 0 .. 16 MiB, page-aligned. Added above the
/// page-aligned end of the ELF image, clamped by the caller to stay under
/// USER_BRK_MAX (64 MiB) so real heap room remains.
inline uint64_t aslr_brk_offset() {
    return g_random.next64() & 0xFFF000ULL;  // 0 .. 16 MiB - 4K
}

}  // namespace cinux::lib
