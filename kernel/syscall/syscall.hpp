/**
 * @file    syscall.hpp
 * @brief   The numbered door ring 3 knocks on.
 *
 * Numbers follow Linux x86_64, registers follow Linux x86_64, errors
 * come back as negative errno — the three conventions a future musl
 * (and through it, any rootfs) assumes without asking. The entry
 * trampoline lives in syscall.S; this file is the table it dispatches
 * into and the frame contract both sides must agree on. Handlers
 * receive raw words and translate for themselves: the kernel does not
 * trust a user pointer until it has been checked against the user
 * half.
 *
 * @author  Charliechen114514
 * @date    2026-10-07
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_syscall
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace cinux::syscall {

/**
 * @brief   Syscall numbers, matching Linux x86_64.
 * @note    Matching the host ABI of busybox-land is the whole point:
 *          a static musl binary issues these numbers verbatim.
 * @since   0.1.0
 * @ingroup kernel_syscall
 */
// NOLINTNEXTLINE(performance-enum-size)
enum class SyscallNr : std::uint16_t {
    kRead  = 0,   ///< read(fd, buf, count)
    kWrite = 1,   ///< write(fd, buf, count)
    kYield = 24,  ///< cooperative yield to the scheduler
    kExit  = 60,  ///< exit(code), never returns
};

/// Linux errno values the early handlers can produce.
inline constexpr int kEnosys = 38;  ///< No such syscall at this number.
inline constexpr int kEbadf  = 9;   ///< Not a descriptor the kernel serves.
inline constexpr int kEfault = 14;  ///< The buffer does not live in the user half.

/// One past the highest served number; the dispatch table spans it.
constexpr unsigned long long kSyscallCount = 61;

/// The lowest address that is none of ring 3's business.
constexpr unsigned long long kUserAddressLimit = 0x0000800000000000ULL;

/**
 * @brief   The frame the entry trampoline builds on the kernel stack.
 * @note    Offset order is the contract with syscall.S; the asserts
 *          pin the layout so C++ and assembly cannot drift. user_rsp
 *          rides at the top so the exit path can rebuild RSP from the
 *          frame itself — gs:8 only stages it on the way in, because
 *          another ring-3 task's entry would overwrite it in between.
 * @since   0.1.0
 * @ingroup kernel_syscall
 */
struct SyscallFrame {
    unsigned long long user_rsp;  ///< +0,   the caller's stack.
    unsigned long long rcx;       ///< +8,   return rip, banked by SYSCALL.
    unsigned long long r11;       ///< +16,  rflags, banked by SYSCALL.
    unsigned long long rax;       ///< +24,  the syscall number.
    unsigned long long rdi;       ///< +32,  first argument.
    unsigned long long rsi;       ///< +40,  second argument.
    unsigned long long rdx;       ///< +48,  third argument.
    unsigned long long r10;       ///< +56,  fourth argument, unserved yet.
    unsigned long long r8;        ///< +64,  fifth argument, unserved yet.
    unsigned long long r9;        ///< +72,  sixth argument, unserved yet.
    unsigned long long rbx;       ///< +80,  callee-saved, restored on exit.
    unsigned long long rbp;       ///< +88,  callee-saved, restored on exit.
};

static_assert(offsetof(SyscallFrame, rcx) == 8, "entry banks rcx at +8");
static_assert(offsetof(SyscallFrame, rax) == 24, "the number sits at +24");
static_assert(offsetof(SyscallFrame, rbx) == 80, "the rbx lesson stays pinned");
static_assert(sizeof(SyscallFrame) == 96, "twelve quadwords of frame");

}  // namespace cinux::syscall

extern "C" {

/**
 * @brief         The LSTAR target: ring 3 arrives here on SYSCALL.
 * @since         0.1.0
 * @ingroup       kernel_syscall
 */
void SyscallEntry();

/**
 * @brief         Run one numbered syscall; the C side of the door.
 * @param[in]     number   Which syscall, from RAX at entry.
 * @param[in]     arg1     First word argument.
 * @param[in]     arg2     Second word argument.
 * @param[in]     arg3     Third word argument.
 * @return        The value ring 3 receives in RAX: a count, or a
 *                negative errno on refusal.
 * @note          C linkage for the assembly caller. Handlers may
 *                switch tasks inside (yield, exit, blocking read);
 *                the frame on the task's kernel stack is what makes
 *                that survivable.
 * @since         0.1.0
 * @ingroup       kernel_syscall
 */
long long InvokeSyscall(unsigned long long number, unsigned long long arg1, unsigned long long arg2,
                        unsigned long long arg3);

}  // extern "C"
