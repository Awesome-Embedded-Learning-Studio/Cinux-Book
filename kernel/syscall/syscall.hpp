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

#include "kernel/fs/fs_config.hpp"

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
    kRead          = 0,    ///< read(fd, buf, count)
    kWrite         = 1,    ///< write(fd, buf, count)
    kOpen          = 2,    ///< open(path, flags)
    kClose         = 3,    ///< close(fd)
    kStat          = 6,    ///< stat(path, out): one node's metadata
    kMmap          = 9,    ///< mmap(hint, len, prot, flags, fd, offset)
    kMunmap        = 10,   ///< munmap(addr, len)
    kMprotect      = 11,   ///< mprotect(addr, len, prot)
    kBrk           = 12,   ///< brk(target): returns the break, not an errno
    kRtSigaction   = 13,   ///< rt_sigaction(sig, act, oact, size): install a handler
    kRtSigprocmask = 14,   ///< rt_sigprocmask(how, set, oldset, size): move the mask
    kRtSigreturn   = 15,   ///< rt_sigreturn(): the trampoline's way home
    kYield         = 24,   ///< cooperative yield to the scheduler
    kGetpid        = 39,   ///< getpid(): the caller's own identity
    kClone         = 56,   ///< clone(flags, stack, ...): fork with a say in what stays shared
    kFork          = 57,   ///< fork(): one call, two returns
    kExecve        = 59,   ///< execve(path): replace this world with a program
    kExit          = 60,   ///< exit(code), never returns
    kWait4         = 61,   ///< wait4(pid, status): block for one child
    kKill          = 62,   ///< kill(pid, sig): deliver a signal to one task
    kSetpgid       = 109,  ///< setpgid(pid, pgid): move a task into a group
    kSetsid        = 112,  ///< setsid(): become a session of one's own
    kGetpgid       = 121,  ///< getpgid(pid): read a task's group
    kGetsid        = 124,  ///< getsid(pid): read a task's session
    kGetppid       = 110,  ///< getppid(): who the caller may be waited on by
    kGetcwd        = 79,   ///< getcwd(buf, size): the caller's working directory
    kChdir         = 80,   ///< chdir(path): move the caller's working directory
    kReboot        = 169,  ///< reboot(): power the machine off through ACPI
    kMkdir         = 83,   ///< mkdir(path)
    kUnlink        = 87,   ///< unlink(path)
    kGetdents      = 217,  ///< getdents(fd, entry): one directory entry per call
    kArchPrctl     = 158,  ///< arch_prctl(op, addr): move a thread-pointer base
    kGettid        = 186,  ///< gettid(): the caller's own thread identity
    kFutex         = 202,  ///< futex(uaddr, op, val): park or wake on a user word
    kSetTidAddress = 218,  ///< set_tid_address(addr): name the word to clear at death
};

/// Linux errno values the handlers can produce.
inline constexpr int kEperm        = 1;   ///< The caller may not do this to the target.
inline constexpr int kEnoent       = 2;   ///< No such file or directory.
inline constexpr int kEsrch        = 3;   ///< No such task.
inline constexpr int kEintr        = 4;   ///< The call was interrupted by a signal.
inline constexpr int kEio          = 5;   ///< The volume read came back short.
inline constexpr int kEbadf        = 9;   ///< Not a descriptor the kernel serves.
inline constexpr int kEchild       = 10;  ///< The wait names no child of the caller.
inline constexpr int kEagain       = 11;  ///< Try again; nothing is ready right now.
inline constexpr int kEnomem       = 12;  ///< The kernel has no room for this.
inline constexpr int kEfault       = 14;  ///< The buffer does not live in the user half.
inline constexpr int kEexist       = 17;  ///< The name already exists.
inline constexpr int kEnotdir      = 20;  ///< A path component is a file.
inline constexpr int kEisdir       = 21;  ///< A file-only operation met a directory.
inline constexpr int kEinval       = 22;  ///< Unusable argument.
inline constexpr int kEpipe        = 32;  ///< The pipe's reader is gone.
inline constexpr int kErange       = 34;  ///< The output buffer is too small.
inline constexpr int kEnametoolong = 36;  ///< The folded path overflows its buffer.
inline constexpr int kEnosys       = 38;  ///< No such syscall at this number.
inline constexpr int kEnotempty    = 39;  ///< The directory still holds entries.

/// open() flag the kernel serves: create the file when it is missing.
inline constexpr unsigned long long kOpenCreat = 0x40;

/// clone() flags, Linux-shaped. The served four overlay the fork
/// child; the tid-word trio arms the join handshake the death side
/// of futex answers.
inline constexpr unsigned long long kCloneVm     = 0x100;    ///< Share the address space.
inline constexpr unsigned long long kCloneFiles  = 0x400;    ///< Share the descriptor table.
inline constexpr unsigned long long kCloneThread = 0x10000;  ///< Join the maker's thread group.
inline constexpr unsigned long long kCloneSetTls = 0x80000;  ///< Arm the child's thread pointer.
inline constexpr unsigned long long kCloneParentSetTid =
    0x100000;  ///< Write the child tid for the maker.
inline constexpr unsigned long long kCloneChildClearTid =
    0x200000;  ///< Clear a word at child death.
inline constexpr unsigned long long kCloneChildSetTid =
    0x1000000;  ///< Write the child tid for the child.

/// mmap protection bits, Linux-shaped.
inline constexpr unsigned long long kProtRead  = 0x1;  ///< Loads through the range promised.
inline constexpr unsigned long long kProtWrite = 0x2;  ///< Stores through the range promised.
inline constexpr unsigned long long kProtExec  = 0x4;  ///< Running code from the range promised.

/// mmap flags. Private anonymous is the shape the kernel serves; a fixed
/// address is refused rather than half-honored.
inline constexpr unsigned long long kMapFixed     = 0x10;
inline constexpr unsigned long long kMapAnonymous = 0x20;

/// One past the highest served number; the dispatch table spans it.
constexpr unsigned long long kSyscallCount = 219;

/// The lowest address that is none of ring 3's business.
constexpr unsigned long long kUserAddressLimit = 0x0000800000000000ULL;

/**
 * @brief   One directory entry, as getdents hands it to ring 3.
 * @note    One entry per call instead of Linux's packed buffer — the
 *          teaching shape; the number and the semantics match, the
 *          packing can follow when busybox needs it.
 * @since   0.1.0
 * @ingroup kernel_syscall
 */
struct SyscallDirent {
    char          name[cinux::fs::kFsNameMax];  ///< NUL-terminated entry name.
    unsigned char type;                         ///< kDirentFile or kDirentDirectory.
};

/// getdents' type word for a regular file.
inline constexpr unsigned char kDirentFile = 0;

/// getdents' type word for a directory.
inline constexpr unsigned char kDirentDirectory = 1;

/**
 * @brief   One node's metadata, as stat hands it to ring 3.
 * @note    The file-system's FsStat with the kernel's types spelled in
 *          full words; the half-size stat the concept calls for.
 * @since   0.3.0
 * @ingroup kernel_syscall
 */
struct SyscallStat {
    unsigned long long ino;    ///< Inode number.
    unsigned long long mode;   ///< Type and permission bits, as stored.
    unsigned long long links;  ///< Directory entries naming this node.
    unsigned long long size;   ///< File bytes; directories report their block bytes.
    unsigned long long mtime;  ///< Modification time, Unix seconds.
};

/**
 * @brief   The frame the entry trampoline builds on the kernel stack.
 * @note    Offset order is the contract with syscall.S; the asserts
 *          pin the layout so C++ and assembly cannot drift. user_rsp
 *          rides at the top so the exit path can rebuild RSP from the
 *          frame itself — gs:8 only stages it on the way in, because
 *          another ring-3 task's entry would overwrite it in between.
 *          Every general register rides the frame since the signal
 *          station: a handler delivery rewrites the whole image and
 *          sends the task back to ring 3 from here, so the frame is
 *          the task's user register file while it sits in the kernel.
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
    unsigned long long r12;       ///< +96,  callee-saved, restored on exit.
    unsigned long long r13;       ///< +104, callee-saved, restored on exit.
    unsigned long long r14;       ///< +112, callee-saved, restored on exit.
    unsigned long long r15;       ///< +120, callee-saved, restored on exit.
};

static_assert(offsetof(SyscallFrame, rcx) == 8, "entry banks rcx at +8");
static_assert(offsetof(SyscallFrame, rax) == 24, "the number sits at +24");
static_assert(offsetof(SyscallFrame, rbx) == 80, "the rbx lesson stays pinned");
static_assert(offsetof(SyscallFrame, r12) == 96, "r12 closes the sixteen-word frame");
static_assert(sizeof(SyscallFrame) == 128, "sixteen quadwords of frame");

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
 * @param[in,out] frame   The entry frame: number and every ABI word
 *                        come off it, and its rax is the return slot.
 * @return        The value ring 3 receives in RAX: a count, or a
 *                negative errno on refusal.
 * @note          C linkage for the assembly caller, which hands the
 *                frame over as one pointer. Handlers may switch tasks
 *                inside (yield, exit, blocking read); the frame on
 *                the task's kernel stack is what makes that
 *                survivable.
 * @since         0.1.0
 * @ingroup       kernel_syscall
 */
long long InvokeSyscall(cinux::syscall::SyscallFrame& frame);

}  // extern "C"
