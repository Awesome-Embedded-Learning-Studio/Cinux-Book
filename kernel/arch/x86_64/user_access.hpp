/**
 * @file kernel/arch/x86_64/user_access.hpp
 * @brief SMAP-aware user-memory accessors (Linux uaccess.h aligned)
 *
 * CinuxOS SMAP model (see document/ai for the design that replaced F9 batch 4's
 * broken global-STAC approach):
 *   - CR4.SMAP is set, so by default (RFLAGS.AC = 0) the kernel CANNOT read or
 *     write user pages. Any direct dereference of a user pointer from kernel
 *     code is a bug and will #PF.
 *   - Kernel code that legitimately touches user memory (syscalls reading
 *     user args / writing user results, signal frame delivery, execve reading
 *     argv/envp) MUST go through these accessors. Each accessor raises AC only
 *     for the copy window via stac(), then clears it with clac().
 *   - RFLAGS.AC is a per-CPU bit and context_switch does NOT save RFLAGS, so an
 *     accessor window must NEVER block/schedule. Callers that need to block
 *     (blocking read, waitpid) stage data in a kernel buffer while blocked and
 *     copy_to_user() only after they are runnable again.
 *
 * Fault handling (F-EXTABLE): copy_to_user / copy_from_user are a single
 * rep movsb whose only faulting instruction is annotated with _ASM_EXTABLE.
 * On a mid-copy #PF the PF handler looks up the fault RIP in the __ex_table
 * and, on a hit, rewrites frame->rip to the fixup -- which runs clac and
 * returns false (caller returns -EFAULT). This is the Linux copy_to_user
 * contract instead of the old demand-page/panic gamble. access_ok() still
 * rejects bad ranges up front; the extable only catches faults inside the
 * annotated copy window (kernel-mode RIP). Host builds keep a plain byte loop
 * (no SMAP, no #PF recovery).
 *
 * Namespace: cinux::arch (stac/clac hardware primitives) + cinux::user
 * (access_ok / copy_to_from_user / put_user / get_user).
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "kernel/arch/x86_64/extable.hpp"        // _ASM_EXTABLE (accessor fault fixup)
#include "kernel/arch/x86_64/paging_config.hpp"  // is_user_vaddr

namespace cinux::arch {

/// Set RFLAGS.AC = 1, allowing kernel access to user pages (SMAP bypass for the
/// accessor window). Per-CPU; pair with clac() and never block in between.
#ifdef CINUX_HOST_TEST
inline void stac() {}  // host unit tests have no SMAP; no-op keeps RFLAGS clean
inline void clac() {}
#else
inline void stac() {
    __asm__ volatile("stac" ::: "memory");
}
inline void clac() {
    __asm__ volatile("clac" ::: "memory");
}
#endif

}  // namespace cinux::arch

namespace cinux::user {

/// True iff [addr, addr+size) lies entirely in the user canonical half
/// (bit 47 = 0). A pure range check -- no fault, no page walk. Rejects NULL,
/// any kernel high-half address, and addr+size wraparound. Stricter than
/// cinux::syscall::validate_user_ptr (which only checks canonical form and
/// admits kernel high-half addresses).
inline bool access_ok(const void* addr, size_t size) {
    uint64_t a = reinterpret_cast<uint64_t>(addr);
    if (a == 0) {
        return false;  // NULL is never a valid user range
    }
    uint64_t end;
    if (__builtin_add_overflow(a, size, &end)) {
        return false;  // addr + size wraps past 2^64
    }
    // end is one-past-the-last byte; the last byte touched is end-1 (or a when
    // size == 0). Both endpoints must be in the user half.
    uint64_t last = (size == 0) ? a : end - 1;
    return cinux::arch::is_user_vaddr(a) && cinux::arch::is_user_vaddr(last);
}

/// Copy @p n bytes kernel -> user. Returns true on success; false (caller
/// returns -EFAULT) if access_ok rejects the range OR a mid-copy #PF hits an
/// unmappable user page. The kernel path is a single rep movsb annotated with
/// _ASM_EXTABLE so a fault resumes at the fixup (clac + ok=false) instead of
/// panicking. The stac/clac window never blocks.
inline __attribute__((always_inline)) bool copy_to_user(void* dst_user, const void* src_kernel,
                                                        size_t n) {
    if (!access_ok(dst_user, n)) {
        return false;
    }
    if (n == 0) {
        return true;
    }
#ifdef CINUX_HOST_TEST
    auto*       d = reinterpret_cast<uint8_t*>(dst_user);
    const auto* s = reinterpret_cast<const uint8_t*>(src_kernel);
    for (size_t i = 0; i < n; ++i) {
        d[i] = s[i];
    }
    return true;
#else
    bool ok = true;
    asm volatile(
        "stac\n"
        "1: rep movsb\n"
        "   clac\n"
        "   jmp 3f\n"
        "2: clac\n"  // fixup: faulted with AC=1 -- close the window, signal failure
        "   xorl %k[ok], %k[ok]\n"
        "3:\n" _ASM_EXTABLE(1b, 2b)
        : [ok] "+r"(ok), "+c"(n), "+D"(dst_user), "+S"(src_kernel)
        :
        : "memory");
    return ok;
#endif
}

/// Copy @p n bytes user -> kernel. Same contract as copy_to_user.
inline __attribute__((always_inline)) bool copy_from_user(void* dst_kernel, const void* src_user,
                                                          size_t n) {
    if (!access_ok(src_user, n)) {
        return false;
    }
    if (n == 0) {
        return true;
    }
#ifdef CINUX_HOST_TEST
    auto*       d = reinterpret_cast<uint8_t*>(dst_kernel);
    const auto* s = reinterpret_cast<const uint8_t*>(src_user);
    for (size_t i = 0; i < n; ++i) {
        d[i] = s[i];
    }
    return true;
#else
    bool ok = true;
    asm volatile(
        "stac\n"
        "1: rep movsb\n"
        "   clac\n"
        "   jmp 3f\n"
        "2: clac\n"  // fixup: faulted with AC=1 -- close the window, signal failure
        "   xorl %k[ok], %k[ok]\n"
        "3:\n" _ASM_EXTABLE(1b, 2b)
        : [ok] "+r"(ok), "+c"(n), "+D"(dst_kernel), "+S"(src_user)
        :
        : "memory");
    return ok;
#endif
}

/// Write a single typed value to user memory (Linux put_user).
template <typename T>
inline __attribute__((always_inline)) bool put_user(T value, T* dst_user) {
    return copy_to_user(dst_user, &value, sizeof(T));
}

/// Read a single typed value from user memory (Linux get_user).
template <typename T>
inline __attribute__((always_inline)) bool get_user(T* dst_kernel, const T* src_user) {
    return copy_from_user(dst_kernel, src_user, sizeof(T));
}

}  // namespace cinux::user
