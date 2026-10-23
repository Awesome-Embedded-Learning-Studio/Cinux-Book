/**
 * @file    signal.hpp
 * @brief   The signal numbers, sets and action words ring 3 and the
 *          kernel agree on.
 *
 * Numbers follow Linux x86_64 (1..22, the trimmed v1 range): a static
 * program raising or masking a signal issues these values verbatim.
 * The set is one word, the action record is four words, and handler
 * addresses use two reserved values (0 default, 1 ignore) the way
 * Linux's SIG_DFL and SIG_IGN do. Everything here is layout the two
 * worlds share, so the file stays free of kernel types.
 *
 * @author  Charliechen114514
 * @date    2026-10-16
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_syscall
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <cstddef>

#include "cinux/bit_ops/bitmask.hpp"

namespace cinux::syscall {

/// Signal numbers, matching Linux x86_64 for the range the kernel serves.
inline constexpr int kSigHup    = 1;   ///< Hangup.
inline constexpr int kSigInt    = 2;   ///< Interrupt from the keyboard.
inline constexpr int kSigQuit   = 3;   ///< Quit from the keyboard.
inline constexpr int kSigIll    = 4;   ///< Illegal instruction.
inline constexpr int kSigTrap   = 5;   ///< Trace trap.
inline constexpr int kSigAbrt   = 6;   ///< Abort.
inline constexpr int kSigBus    = 7;   ///< Bus error.
inline constexpr int kSigFpe    = 8;   ///< Floating-point exception.
inline constexpr int kSigKill   = 9;   ///< Kill; cannot be caught or blocked.
inline constexpr int kSigUsr1   = 10;  ///< User-defined signal one.
inline constexpr int kSigSegv   = 11;  ///< Invalid memory reference.
inline constexpr int kSigUsr2   = 12;  ///< User-defined signal two.
inline constexpr int kSigPipe   = 13;  ///< Write on a pipe with no reader.
inline constexpr int kSigAlrm   = 14;  ///< Timer alarm.
inline constexpr int kSigTerm   = 15;  ///< Termination request.
inline constexpr int kSigStkflt = 16;  ///< Stack fault.
inline constexpr int kSigChld   = 17;  ///< A child stopped or exited.
inline constexpr int kSigCont   = 18;  ///< Continue a stopped task.
inline constexpr int kSigStop   = 19;  ///< Stop; cannot be caught or blocked.
inline constexpr int kSigTstp   = 20;  ///< Stop typed at the keyboard.
inline constexpr int kSigTtin   = 21;  ///< Background read tried the terminal.
inline constexpr int kSigTtou   = 22;  ///< Background write tried the terminal.

/// One past the highest served signal; sets and tables span it.
inline constexpr int kSignalCount = 22;

/// Handler word meaning "take the default action for this signal".
inline constexpr unsigned long long kSignalDefault = 0;

/// Handler word meaning "throw this signal away on arrival".
inline constexpr unsigned long long kSignalIgnore = 1;

/// sa_flags: interrupted syscalls restart where they left off.
inline constexpr unsigned long long kSaRestart = 0x10000000;

/// sa_flags: the action record names its own return trampoline. libc
/// sets this on every handler it installs and points it at a restorer
/// gadget ending in the rt_sigreturn call; honoring it keeps the
/// kernel-planted trampoline page a fallback for freestanding programs
/// that install handlers without one.
inline constexpr unsigned long long kSaRestorer = 0x04000000;

/// sa_flags: the handler reverts to default once it has run once.
inline constexpr unsigned long long kSaResethand = 0x80000000;

/// One word of signal set; bit n names signal n+1. The word carries the
/// base vocabulary's named-bit face, so both worlds read one type and no
/// set-operator is written twice.
using SigSet = cinux::base::bit::BitMask<unsigned long long>;

/**
 * @brief   One signal's disposition, as rt_sigaction installs it.
 * @note    Linux x86_64 rt_sigaction order: handler, flags, restorer,
 *          then the eight-byte kernel signal mask.
 *          The mask rides along so the kernel can hold it while the
 *          handler runs, restoring it at sigreturn.
 * @since   0.1.0
 * @ingroup kernel_syscall
 */
struct SigAction {
    unsigned long long handler{};   ///< Handler address, kSignalDefault or kSignalIgnore.
    unsigned long long flags{};     ///< kSaRestart, kSaResethand, or zero.
    unsigned long long restorer{};  ///< User restorer address; delivery uses the kernel trampoline.
    SigSet             mask;        ///< Extra signals held while the handler runs.
};

static_assert(sizeof(SigAction) == 32, "Linux x86_64 kernel sigaction is four words");
static_assert(offsetof(SigAction, flags) == 8);
static_assert(offsetof(SigAction, restorer) == 16);
static_assert(offsetof(SigAction, mask) == 24);

}  // namespace cinux::syscall
