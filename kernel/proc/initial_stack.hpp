/**
 * @file kernel/proc/initial_stack.hpp
 * @brief Build the Linux x86_64 initial user stack (argc/argv/envp/auxv)
 *
 * Pure, dual-compilable (freestanding kernel + host unit test) helper that lays
 * out the process-entry stack the kernel writes before jumping to a new program:
 *
 *   low VA                                              stack_top (high VA)
 *   [ argc | argv[] | NULL | envp[] | NULL | auxv.. | AT_NULL | strings.. | pad ]
 *
 * The layout is right-aligned in the caller's buffer so that buf[cap-1] maps to
 * user VA stack_top-1: the caller can pass the top stack page's direct-mapped
 * kernel address directly and avoid a separate copy. argv/envp pointers and the
 * AT_RANDOM / AT_EXECFN values are absolute user VAs derived from stack_top.
 * The entry RSP (argc address = stack_top - size) is 16-byte aligned, per the
 * SysV x86_64 ABI (process entry, no return address).
 *
 * musl's __init_libc / __libc_start_main read this layout at startup -- in
 * particular AT_PHDR/AT_PHENT/AT_PHNUM (find PT_TLS), AT_PAGESZ (asserts
 * non-zero), AT_RANDOM (stack canary / SSP init), AT_UID/EUID/GID/EGID +
 * AT_SECURE (credentials), AT_ENTRY. See musl src/env/__libc_start_main.c.
 *
 * Self-contained (no <cstring>): the freestanding kernel build has no standard
 * <cstring>, so the byte helpers are inline here.
 */

#pragma once

#include <stddef.h>

#include <cstdint>

namespace cinux::proc {

// Linux auxiliary-vector types (uapi/linux/auxvec.h); the subset CinuxOS emits.
constexpr uint64_t AT_NULL         = 0;
constexpr uint64_t AT_PHDR         = 3;
constexpr uint64_t AT_PHENT        = 4;
constexpr uint64_t AT_PHNUM        = 5;
constexpr uint64_t AT_PAGESZ       = 6;
constexpr uint64_t AT_BASE         = 7;
constexpr uint64_t AT_FLAGS        = 8;
constexpr uint64_t AT_ENTRY        = 9;
constexpr uint64_t AT_UID          = 11;
constexpr uint64_t AT_EUID         = 12;
constexpr uint64_t AT_GID          = 13;
constexpr uint64_t AT_EGID         = 14;
constexpr uint64_t AT_HWCAP        = 16;
constexpr uint64_t AT_CLKTCK       = 17;
constexpr uint64_t AT_SECURE       = 23;
constexpr uint64_t AT_RANDOM       = 25;
constexpr uint64_t AT_EXECFN       = 31;
constexpr uint64_t AT_SYSINFO_EHDR = 33;

/// One Elf64_auxv_t entry (a_type, a_val).
struct AuxEntry {
    uint64_t type_;
    uint64_t val_;
};

namespace idetail {

inline uint64_t str_len(const char* s) {
    uint64_t n = 0;
    while (s[n] != '\0')
        ++n;
    return n;
}

inline void copy_bytes(uint8_t* dst, const uint8_t* src, uint64_t n) {
    for (uint64_t i = 0; i < n; ++i)
        dst[i] = src[i];
}

inline void put_u64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i)
        p[i] = static_cast<uint8_t>(v >> (8 * i));
}

inline uint64_t get_u64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v |= static_cast<uint64_t>(p[i]) << (8 * i);
    return v;
}

}  // namespace idetail

/// Build the initial user stack right-aligned in buf[0..cap) (buf[cap-1] maps to
/// stack_top-1). @p aux holds the fixed entries the caller computed (AT_PHDR/
/// PHNUM/PAGESZ/ENTRY/UID/...); do NOT include AT_NULL/AT_RANDOM/AT_EXECFN --
/// this helper adds them, since their values depend on the string layout it
/// computes. @p filename becomes AT_EXECFN; @p random16 (16 bytes) -> AT_RANDOM.
///
/// @return bytes used (the layout occupies buf[cap-ret .. cap)); the entry RSP =
/// stack_top - ret is 16-byte aligned. Returns 0 on cap overflow.
inline uint64_t build_initial_stack(uint8_t* buf, uint64_t cap, uint64_t stack_top, uint64_t argc,
                                    const char* const* argv, uint64_t envc, const char* const* envp,
                                    const AuxEntry* aux, uint64_t auxc, const char* filename,
                                    const uint8_t random16[16]) {
    using namespace idetail;

    // 1. Measure the string region: argv, envp, filename, +16 (AT_RANDOM bytes).
    uint64_t str_bytes = 0;
    for (uint64_t i = 0; i < argc; ++i)
        str_bytes += str_len(argv[i]) + 1;
    for (uint64_t i = 0; i < envc; ++i)
        str_bytes += str_len(envp[i]) + 1;
    uint64_t fn_len = str_len(filename) + 1;
    str_bytes += fn_len + 16;

    // 2. Block = argc + argv ptrs + NULL + envp ptrs + NULL + auxv pairs +
    //    AT_NULL pair. The auxv holds the caller's entries plus AT_EXECFN and
    //    AT_RANDOM (2 extra), plus the AT_NULL terminator pair (+1).
    uint64_t total_aux  = auxc + 2;
    uint64_t block_size = 8 * (1 + argc + 1 + envc + 1) + 16 * (total_aux + 1);

    uint64_t raw  = block_size + str_bytes;
    uint64_t size = (raw + 15) & ~static_cast<uint64_t>(15);  // RSP % 16 == 0
    if (size == 0 || size > cap)
        return 0;
    uint64_t base = cap - size;

    // Zero the layout region (padding + any bytes the structure leaves gap).
    for (uint64_t i = base; i < cap; ++i)
        buf[i] = 0;

    // buf[cap-1] -> VA stack_top-1, so buf offset o -> VA (stack_top - cap + o).
    const uint64_t bias = stack_top - cap;
    auto           va   = [bias](uint64_t off) { return bias + off; };

    uint64_t off     = base;               // block write cursor (grows up)
    uint64_t str_cur = base + block_size;  // string write cursor (grows up)

    put_u64(buf + off, argc);
    off += 8;
    for (uint64_t i = 0; i < argc; ++i) {
        uint64_t len = str_len(argv[i]) + 1;
        put_u64(buf + off, va(str_cur));
        off += 8;
        copy_bytes(buf + str_cur, reinterpret_cast<const uint8_t*>(argv[i]), len);
        str_cur += len;
    }
    put_u64(buf + off, 0);  // argv terminator
    off += 8;
    for (uint64_t i = 0; i < envc; ++i) {
        uint64_t len = str_len(envp[i]) + 1;
        put_u64(buf + off, va(str_cur));
        off += 8;
        copy_bytes(buf + str_cur, reinterpret_cast<const uint8_t*>(envp[i]), len);
        str_cur += len;
    }
    put_u64(buf + off, 0);  // envp terminator
    off += 8;
    for (uint64_t i = 0; i < auxc; ++i) {
        put_u64(buf + off, aux[i].type_);
        off += 8;
        put_u64(buf + off, aux[i].val_);
        off += 8;
    }
    // AT_EXECFN -> filename string.
    put_u64(buf + off, AT_EXECFN);
    off += 8;
    put_u64(buf + off, va(str_cur));
    off += 8;
    copy_bytes(buf + str_cur, reinterpret_cast<const uint8_t*>(filename), fn_len);
    str_cur += fn_len;
    // AT_RANDOM -> 16 random bytes.
    put_u64(buf + off, AT_RANDOM);
    off += 8;
    put_u64(buf + off, va(str_cur));
    off += 8;
    copy_bytes(buf + str_cur, random16, 16);
    str_cur += 16;
    // AT_NULL terminator pair.
    put_u64(buf + off, AT_NULL);
    off += 8;
    put_u64(buf + off, 0);
    off += 8;

    // off == base + block_size; str_cur == base + raw. buf[base+raw .. cap) is pad.
    return size;
}

}  // namespace cinux::proc
