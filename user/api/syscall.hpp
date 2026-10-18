/**
 * @file    syscall.hpp
 * @brief   The three-instruction bridge from ring 3 into the kernel.
 *
 * Not a libc and not trying to be one: each wrapper is one SYSCALL
 * instruction with the Linux x86_64 register contract, nothing more.
 * The numbers come from the kernel's own header, so both worlds read
 * one table. When musl arrives it brings its own wrappers and this
 * file retires — the ABI it speaks is already ours.
 *
 * @author  Charliechen114514
 * @date    2026-10-11
 * @version 0.2
 * @since   0.1.0
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "kernel/syscall/signal.hpp"
#include "kernel/syscall/syscall.hpp"

namespace user {

/**
 * @brief         Issue one syscall; the ABI constant, spelled once.
 * @param[in]     number   Syscall number, lands in RAX.
 * @param[in]     arg1     First argument, lands in RDI.
 * @param[in]     arg2     Second argument, lands in RSI.
 * @param[in]     arg3     Third argument, lands in RDX.
 * @return        Whatever the kernel hands back in RAX.
 * @note          RCX and R11 are banked by the instruction itself;
 *                everything else the ABI calls caller-saved is listed
 *                as clobbered so the compiler cannot stash a live
 *                value across the hop.
 * @since         0.1.0
 */
inline long long Syscall3(unsigned long long number, unsigned long long arg1,
                          unsigned long long arg2, unsigned long long arg3) {
    // NOLINTNEXTLINE(misc-const-correctness)
    long long ret = 0;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(number), "D"(arg1), "S"(arg2), "d"(arg3)
                     : "rcx", "r11", "memory");
    return ret;
}

/**
 * @brief         write(fd, buf, count): bytes out through the console.
 * @param[in]     descriptor  Destination, 1 for the console.
 * @param[in]     buffer      Where the bytes come from.
 * @param[in]     count       How many.
 * @return        Bytes written, or a negative errno.
 * @since         0.1.0
 */
inline long long Write(unsigned long long descriptor, const void* buffer,
                       unsigned long long count) {
    return Syscall3(static_cast<unsigned long long>(cinux::syscall::SyscallNr::kWrite), descriptor,
                    reinterpret_cast<unsigned long long>(buffer), count);
}

/**
 * @brief         read(fd, buf, count): bytes in, blocking until they exist.
 * @param[in]     descriptor  Source, 0 for the keyboard.
 * @param[in]     buffer      Where the bytes land.
 * @param[in]     count       How many to ask for.
 * @return        Bytes read, or a negative errno.
 * @since         0.1.0
 */
inline long long Read(unsigned long long descriptor, const void* buffer, unsigned long long count) {
    return Syscall3(static_cast<unsigned long long>(cinux::syscall::SyscallNr::kRead), descriptor,
                    reinterpret_cast<unsigned long long>(buffer), count);
}

/**
 * @brief         yield(): hand the CPU to the scheduler for a turn.
 * @return        Zero on the happy path.
 * @since         0.1.0
 */
inline long long Yield() {
    return Syscall3(static_cast<unsigned long long>(cinux::syscall::SyscallNr::kYield), 0, 0, 0);
}

/**
 * @brief         open(path, flags): claim a descriptor for a path.
 * @param[in]     path   Absolute path; the file system serves it.
 * @param[in]     flags  Zero to open, kOpenCreat to create when missing.
 * @return        The descriptor (from 3 up), or a negative errno.
 * @since         0.1.0
 */
inline long long Open(const char* path, unsigned long long flags) {
    return Syscall3(static_cast<unsigned long long>(cinux::syscall::SyscallNr::kOpen),
                    reinterpret_cast<unsigned long long>(path), flags, 0);
}

/**
 * @brief         close(fd): give a descriptor back.
 * @param[in]     descriptor   The number open handed out.
 * @return        Zero, or a negative errno.
 * @since         0.1.0
 */
inline long long Close(unsigned long long descriptor) {
    return Syscall3(static_cast<unsigned long long>(cinux::syscall::SyscallNr::kClose), descriptor,
                    0, 0);
}

/**
 * @brief         mkdir(path): create a directory.
 * @param[in]     path   Absolute path of the new directory.
 * @return        Zero, or a negative errno.
 * @since         0.1.0
 */
inline long long Mkdir(const char* path) {
    return Syscall3(static_cast<unsigned long long>(cinux::syscall::SyscallNr::kMkdir),
                    reinterpret_cast<unsigned long long>(path), 0, 0);
}

/**
 * @brief         unlink(path): remove a file or an empty directory.
 * @param[in]     path   Absolute path of the entry.
 * @return        Zero, or a negative errno.
 * @since         0.1.0
 */
inline long long Unlink(const char* path) {
    return Syscall3(static_cast<unsigned long long>(cinux::syscall::SyscallNr::kUnlink),
                    reinterpret_cast<unsigned long long>(path), 0, 0);
}

/**
 * @brief         getdents(fd, buf, count): read packed directory records.
 * @param[in]     descriptor   An open directory.
 * @param[out]    buf          Receives whole dirent64 records, abutting.
 * @param[in]     count        How many bytes buf holds.
 * @return        The bytes filled, zero at the end, or a negative errno.
 * @since         0.3.0
 */
// NOLINTNEXTLINE(misc-const-correctness) the kernel fills buf; this body only forwards it
inline long long Getdents(unsigned long long descriptor, void* buf, unsigned long long count) {
    return Syscall3(static_cast<unsigned long long>(cinux::syscall::SyscallNr::kGetdents),
                    descriptor, reinterpret_cast<unsigned long long>(buf), count);
}

/**
 * @brief         stat(path, out): one node's metadata.
 * @param[in]     path   The path, relative or absolute.
 * @param[out]    out    Receives the node's metadata.
 * @return        Zero, or a negative errno.
 * @since         0.2.0
 */
// NOLINTNEXTLINE(misc-const-correctness) the kernel fills out; this body only forwards it
inline long long Stat(const char* path, cinux::syscall::SyscallStat* out) {
    return Syscall3(static_cast<unsigned long long>(cinux::syscall::SyscallNr::kStat),
                    reinterpret_cast<unsigned long long>(path),
                    reinterpret_cast<unsigned long long>(out), 0);
}

/**
 * @brief         chdir(path): move the working directory.
 * @param[in]     path   The directory to land in, relative or absolute.
 * @return        Zero, or a negative errno.
 * @since         0.2.0
 */
inline long long Chdir(const char* path) {
    return Syscall3(static_cast<unsigned long long>(cinux::syscall::SyscallNr::kChdir),
                    reinterpret_cast<unsigned long long>(path), 0, 0);
}

/**
 * @brief         getcwd(buf, size): read the working directory.
 * @param[out]    buffer   Receives the NUL-terminated absolute path.
 * @param[in]     size     Bytes writable at buffer, NUL included.
 * @return        Bytes written without the NUL, or a negative errno.
 * @since         0.2.0
 */
// NOLINTNEXTLINE(misc-const-correctness) the kernel writes the path via the syscall
inline long long Getcwd(char* buffer, unsigned long long size) {
    return Syscall3(static_cast<unsigned long long>(cinux::syscall::SyscallNr::kGetcwd),
                    reinterpret_cast<unsigned long long>(buffer), size, 0);
}

/**
 * @brief         reboot(): power the machine off.
 * @return        Zero when the write went out, or a negative errno.
 * @since         0.3.0
 */
inline long long Reboot() {
    return Syscall3(static_cast<unsigned long long>(cinux::syscall::SyscallNr::kReboot), 0, 0, 0);
}

/// exit(code): end this task; never returns.
[[noreturn]] inline void Exit(unsigned long long code) {
    static_cast<void>(
        Syscall3(static_cast<unsigned long long>(cinux::syscall::SyscallNr::kExit), code, 0, 0));
    __builtin_unreachable();
}

/**
 * @brief         getpid(): the caller's own process identity.
 * @return        The thread group leader's pid; every thread of one
 *                process reads the same number here.
 * @since         0.3.0
 */
inline long long Getpid() {
    return Syscall3(static_cast<unsigned long long>(cinux::syscall::SyscallNr::kGetpid), 0, 0, 0);
}

/**
 * @brief         gettid(): the caller's own thread identity.
 * @return        The tid; unique per thread, so inside a thread group
 *                it tells the members apart.
 * @since         0.8.0
 */
inline long long Gettid() {
    return Syscall3(static_cast<unsigned long long>(cinux::syscall::SyscallNr::kGettid), 0, 0, 0);
}

/**
 * @brief         arch_prctl(op, addr): move a thread-pointer base.
 * @param[in]     op     One of the ARCH_SET_FS/ARCH_GET_FS/ARCH_SET_GS/
 *                       ARCH_GET_GS operations.
 * @param[in]     addr   The new base, or the quadword that receives
 *                       the old one.
 * @return        Zero on success, or a negative errno.
 * @since         0.8.0
 */
inline long long ArchPrctl(unsigned long long operation, unsigned long long addr) {
    return Syscall3(static_cast<unsigned long long>(cinux::syscall::SyscallNr::kArchPrctl),
                    operation, addr, 0);
}

/**
 * @brief         set_tid_address(addr): name the word the kernel
 *                clears when this task dies.
 * @param[in]     addr   The user quadword reserved for the joiner.
 * @return        The caller's tid.
 * @since         0.8.0
 */
inline long long SetTidAddress(unsigned long long addr) {
    return Syscall3(static_cast<unsigned long long>(cinux::syscall::SyscallNr::kSetTidAddress),
                    addr, 0, 0);
}

/**
 * @brief         futex(uaddr, FUTEX_WAIT, expected): park while the
 *                word still holds expected.
 * @param[in]     uaddr      The four-byte word to wait on.
 * @param[in]     expected   The value the caller saw before parking.
 * @return        Zero once woken, -11 (EAGAIN) when the word had
 *                already moved on.
 * @since         0.8.0
 */
inline long long FutexWait(unsigned long long uaddr, unsigned int expected) {
    return Syscall3(static_cast<unsigned long long>(cinux::syscall::SyscallNr::kFutex), uaddr, 0,
                    expected);
}

/**
 * @brief         futex(uaddr, FUTEX_WAKE, count): wake parked waiters.
 * @param[in]     uaddr    The four-byte word the waiters parked on.
 * @param[in]     count    At most this many waiters are woken.
 * @return        How many were actually woken.
 * @since         0.8.0
 */
inline long long FutexWake(unsigned long long uaddr, unsigned long long count) {
    return Syscall3(static_cast<unsigned long long>(cinux::syscall::SyscallNr::kFutex), uaddr, 1,
                    count);
}

/**
 * @brief         fork(): one call, two returns.
 * @return        The child's pid to the parent, 0 to the child, or a
 *                negative errno.
 * @since         0.3.0
 */
inline long long Fork() {
    return Syscall3(static_cast<unsigned long long>(cinux::syscall::SyscallNr::kFork), 0, 0, 0);
}

/**
 * @brief         clone(flags, stack, tls, entry, arg, tid words): a
 *                new thread of this process, or a new process shaped
 *                to order.
 * @param[in]     flags       The share-or-copy bits (kCloneVm and
 *                            friends, kernel header); what stays the
 *                            maker's instead of being copied.
 * @param[in]     child_stack First stack pointer the child grows down
 *                            from; its bottom word must be free.
 * @param[in]     tls         Thread pointer for kCloneSetTls.
 * @param[in]     entry       First function the child runs.
 * @param[in]     arg         Lands in rdi for entry.
 * @param[in]     parent_tid  Word the child's tid is written to for the
 *                            maker, under kCloneParentSetTid; pass 0
 *                            otherwise.
 * @param[in]     child_tid   Word the child's tid is written to for
 *                            the child (kCloneChildSetTid) and zeroed
 *                            with one wake at its death
 *                            (kCloneChildClearTid); pass 0 otherwise.
 * @return        The child's tid to the maker, or a negative errno;
 *                the child never reads a return from here.
 * @note          The child lands on the empty given stack at the same
 *                instruction, where a C epilogue would pop garbage —
 *                so this one asm owns both sides: the maker falls
 *                through the label and returns, the child re-points
 *                rdi and calls entry, which ends in Exit and never
 *                comes back. rdx and r10 carry the tid words in the
 *                Linux slots; r9 and r15 carry entry and arg through
 *                the kernel, which banks every register but rax.
 * @since         0.8.0
 */
// NOLINTNEXTLINE(readability-function-size) the child path lives here by necessity
inline long long Clone(unsigned long long flags, const void* child_stack, unsigned long long tls,
                       void (*entry)(void*), const void* arg, unsigned long long parent_tid,
                       unsigned long long child_tid) {
    // NOLINTBEGIN(misc-const-correctness) the syscall writes ret in the maker
    long long                         ret      = 0;
    const auto                        kEntryAt = reinterpret_cast<unsigned long long>(entry);
    const auto                        kArgAt   = reinterpret_cast<unsigned long long>(arg);
    register const unsigned long long kChildTid __asm__("r10") = child_tid;
    register const unsigned long long kTls __asm__("r8")       = tls;
    register const unsigned long long kEntry __asm__("r9")     = kEntryAt;
    register const unsigned long long kArg __asm__("r15")      = kArgAt;
    // NOLINTEND(misc-const-correctness)
    __asm__ volatile(
        "syscall\n\t"
        "testl %%eax, %%eax\n\t"
        "jnz 1f\n\t"
        "xorl %%ebp, %%ebp\n\t"
        "movq %%r9, %%rax\n\t"
        "movq %%r15, %%rdi\n\t"
        "andq $-16, %%rsp\n\t"
        "call *%%rax\n\t"
        "ud2\n"
        "1:\n"
        : "=a"(ret)
        : "a"(static_cast<unsigned long long>(cinux::syscall::SyscallNr::kClone)), "D"(flags),
          "S"(child_stack), "d"(parent_tid), "r"(kChildTid), "r"(kTls), "r"(kEntry), "r"(kArg)
        : "rcx", "r11", "memory");
    return ret;
}

/**
 * @brief         execve(path): replace this world with a program.
 * @param[in]     path   The program's absolute path on the volume.
 * @return        Never comes back when the program takes over; a
 *                refused exec hands back a negative errno and the old
 *                world keeps running.
 * @since         0.3.0
 */
inline long long Execve(const char* path) {
    return Syscall3(static_cast<unsigned long long>(cinux::syscall::SyscallNr::kExecve),
                    reinterpret_cast<unsigned long long>(path), 0, 0);
}

/**
 * @brief         getppid(): who the caller may be waited on by.
 * @return        The parent's pid, zero when parentless.
 * @since         0.3.0
 */
inline long long Getppid() {
    return Syscall3(static_cast<unsigned long long>(cinux::syscall::SyscallNr::kGetppid), 0, 0, 0);
}

/**
 * @brief         wait4(pid, status): block for one child, then retire it.
 * @param[in]     pid    The child's pid, or -1 for any child.
 * @param[out]    where  Receives the child's raw exit code; null skips.
 * @return        The reaped child's pid, or a negative errno.
 * @since         0.3.0
 */
// NOLINTNEXTLINE(misc-const-correctness) the kernel writes the code via the syscall
inline long long Wait4(long long pid, int* where) {
    return Syscall3(static_cast<unsigned long long>(cinux::syscall::SyscallNr::kWait4),
                    static_cast<unsigned long long>(pid),
                    reinterpret_cast<unsigned long long>(where), 0);
}

/**
 * @brief         Issue one syscall with four arguments.
 * @param[in]     number   Syscall number, lands in RAX.
 * @param[in]     arg1     Lands in RDI.
 * @param[in]     arg2     Lands in RSI.
 * @param[in]     arg3     Lands in RDX.
 * @param[in]     arg4     Lands in R10.
 * @return        Whatever the kernel hands back in RAX.
 * @since         0.5.0
 */
// NOLINTNEXTLINE(readability-function-size) the syscall's own shape
inline long long Syscall4(unsigned long long number, unsigned long long arg1,
                          unsigned long long arg2, unsigned long long arg3,
                          unsigned long long arg4) {
    // NOLINTNEXTLINE(misc-const-correctness)
    long long                         ret                    = 0;
    register const unsigned long long kFourth __asm__("r10") = arg4;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(number), "D"(arg1), "S"(arg2), "d"(arg3), "r"(kFourth)
                     : "rcx", "r11", "memory");
    return ret;
}

/**
 * @brief         wait4 with WNOHANG: reap a child only if one is ready.
 * @param[in]     pid    As with Wait4; -1 names every child, a group
 *                       number below -1 names that group's children.
 * @param[out]    where  Receives the exit status, or null.
 * @return        The reaped child's pid; zero when the wanted children
 *                all live on (nothing changed state), or a negative
 *                errno. The status word is only meaningful above zero.
 * @since         0.7.0
 */
inline long long Wait4NoHang(
    long long pid,
    int*      where  // NOLINT(misc-const-correctness) the kernel writes it via the syscall
) {
    return Syscall4(static_cast<unsigned long long>(cinux::syscall::SyscallNr::kWait4),
                    static_cast<unsigned long long>(pid),
                    reinterpret_cast<unsigned long long>(where), 1, 0);
}

/**
 * @brief         kill(pid, sig): deliver a signal to a task or a group.
 * @param[in]     pid     Above zero names one task; zero names every
 *                        task in the caller's own group; -1 names
 *                        every other task; below -1 names the group
 *                        at -pid.
 * @param[in]     number  The signal, or zero to only probe existence.
 * @return        Zero when accepted, or a negative errno.
 * @since         0.5.0
 */
inline long long Kill(unsigned long long pid, int number) {
    return Syscall3(static_cast<unsigned long long>(cinux::syscall::SyscallNr::kKill), pid,
                    static_cast<unsigned long long>(number), 0);
}

/**
 * @brief         setpgid(pid, pgid): move a task into a group.
 * @param[in]     pid    The target; zero means the caller, and a
 *                       caller may only move itself or its own child.
 * @param[in]     pgid   The group to join; zero means the target's
 *                       own pid, and any other value must already
 *                       name a group in the target's session.
 * @return        Zero, or a negative errno naming why the move was
 *                refused.
 * @since         0.5.0
 */
inline long long Setpgid(unsigned long long pid, unsigned long long pgid) {
    return Syscall3(static_cast<unsigned long long>(cinux::syscall::SyscallNr::kSetpgid), pid, pgid,
                    0);
}

/**
 * @brief         setsid(): become a session of one's own.
 * @return        The new session id (the caller's pid), or a negative
 *                errno when the caller already leads a group.
 * @since         0.5.0
 */
inline long long Setsid() {
    return Syscall3(static_cast<unsigned long long>(cinux::syscall::SyscallNr::kSetsid), 0, 0, 0);
}

/**
 * @brief         getpgid(pid): read a task's group.
 * @param[in]     pid    The target; zero means the caller.
 * @return        The group id, or a negative errno.
 * @since         0.5.0
 */
inline long long Getpgid(unsigned long long pid) {
    return Syscall3(static_cast<unsigned long long>(cinux::syscall::SyscallNr::kGetpgid), pid, 0,
                    0);
}

/**
 * @brief         getsid(pid): read a task's session.
 * @param[in]     pid    The target; zero means the caller.
 * @return        The session id, or a negative errno.
 * @since         0.5.0
 */
inline long long Getsid(unsigned long long pid) {
    return Syscall3(static_cast<unsigned long long>(cinux::syscall::SyscallNr::kGetsid), pid, 0, 0);
}

/**
 * @brief         rt_sigaction(sig, act, oact, size): install a handler.
 * @param[in]     number   The signal to arm.
 * @param[in]     action   The new disposition, or null to only ask.
 * @param[out]    old      Receives the previous disposition, or null.
 * @return        Zero, or a negative errno.
 * @since         0.5.0
 */
inline long long Sigaction(int number, const cinux::syscall::SigAction* action,
                           cinux::syscall::SigAction* old  // NOLINT(misc-const-correctness) the
                                                           // kernel writes it via the syscall
) {
    return Syscall4(static_cast<unsigned long long>(cinux::syscall::SyscallNr::kRtSigaction),
                    static_cast<unsigned long long>(number),
                    reinterpret_cast<unsigned long long>(action),
                    reinterpret_cast<unsigned long long>(old), sizeof(cinux::syscall::SigSet));
}

/**
 * @brief         rt_sigprocmask(how, set, old, size): move the mask.
 * @param[in]     how   0 blocks the set, 1 unblocks it, 2 replaces the mask.
 * @param[in]     set   The set to apply, or null to only ask.
 * @param[out]    old   Receives the previous mask, or null.
 * @return        Zero, or a negative errno.
 * @since         0.5.0
 */
inline long long Sigprocmask(int how, const cinux::syscall::SigSet* set,
                             cinux::syscall::SigSet* old  // NOLINT(misc-const-correctness) the
                                                          // kernel writes it via the syscall
) {
    return Syscall4(static_cast<unsigned long long>(cinux::syscall::SyscallNr::kRtSigprocmask),
                    static_cast<unsigned long long>(how), reinterpret_cast<unsigned long long>(set),
                    reinterpret_cast<unsigned long long>(old), sizeof(cinux::syscall::SigSet));
}

/**
 * @brief         Issue one syscall with the full six-argument contract.
 * @param[in]     number   Syscall number, lands in RAX.
 * @param[in]     arg1     Lands in RDI.
 * @param[in]     arg2     Lands in RSI.
 * @param[in]     arg3     Lands in RDX.
 * @param[in]     arg4     Lands in R10.
 * @param[in]     arg5     Lands in R8.
 * @param[in]     arg6     Lands in R9.
 * @return        Whatever the kernel hands back in RAX.
 * @note          The fourth argument is where this leaves the
 *                three-register door: R10 instead of RCX, because the
 *                syscall instruction banks the real RCX itself.
 * @since         0.4.0
 */
// NOLINTNEXTLINE(readability-function-size) the ABI face itself is seven words wide
inline long long Syscall6(unsigned long long number, unsigned long long arg1,
                          unsigned long long arg2, unsigned long long arg3, unsigned long long arg4,
                          unsigned long long arg5, unsigned long long arg6) {
    // NOLINTNEXTLINE(misc-const-correctness)
    long long                         ret                    = 0;
    register const unsigned long long kFourth __asm__("r10") = arg4;
    register const unsigned long long kFifth __asm__("r8")   = arg5;
    register const unsigned long long kSixth __asm__("r9")   = arg6;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(number), "D"(arg1), "S"(arg2), "d"(arg3), "r"(kFourth), "r"(kFifth),
                       "r"(kSixth)
                     : "rcx", "r11", "memory");
    return ret;
}

/**
 * @brief         mmap(hint, len, prot, flags, fd, offset): promise a range.
 * @param[in]     hint        Ignored; the kernel hunts for the spot.
 * @param[in]     length      Bytes wanted, rounded up to whole pages.
 * @param[in]     prot        kProtRead/kProtWrite/kProtExec bits.
 * @param[in]     flags       kMapAnonymous for fresh zero pages; a file
 *                            mapping passes zero and names the fd.
 * @param[in]     descriptor  The file to map, unused when anonymous.
 * @param[in]     offset      Byte offset into the file.
 * @return        First byte of the promised range, or a negative errno.
 * @note          Nothing is mapped yet; the first touch faults the
 *                pages in.
 * @since         0.4.0
 */
// NOLINTNEXTLINE(readability-function-size) the mmap ABI is six arguments wide by contract
inline long long Mmap(unsigned long long hint, unsigned long long length, unsigned long long prot,
                      unsigned long long flags, unsigned long long descriptor,
                      unsigned long long offset) {
    return Syscall6(static_cast<unsigned long long>(cinux::syscall::SyscallNr::kMmap), hint, length,
                    prot, flags, descriptor, offset);
}

/**
 * @brief         munmap(addr, len): withdraw a promise, drop its pages.
 * @param[in]     address  First byte of the range, as mmap handed it out.
 * @param[in]     length   Bytes to withdraw.
 * @return        Zero, or a negative errno.
 * @since         0.4.0
 */
inline long long Munmap(unsigned long long address, unsigned long long length) {
    return Syscall3(static_cast<unsigned long long>(cinux::syscall::SyscallNr::kMunmap), address,
                    length, 0);
}

/**
 * @brief         mprotect(addr, len, prot): rewrite a range's terms.
 * @param[in]     address  First byte of a live range.
 * @param[in]     length   Bytes to re-protect.
 * @param[in]     prot     The new kProtRead/kProtWrite/kProtExec bits.
 * @return        Zero, or a negative errno.
 * @since         0.4.0
 */
inline long long Mprotect(unsigned long long address, unsigned long long length,
                          unsigned long long prot) {
    return Syscall3(static_cast<unsigned long long>(cinux::syscall::SyscallNr::kMprotect), address,
                    length, prot);
}

/**
 * @brief         brk(target): move the program break.
 * @param[in]     target  Break wanted; zero only asks.
 * @return        The break after the call. Failure reads as "the break
 *                did not move", not as an errno.
 * @since         0.4.0
 */
inline long long Brk(unsigned long long target) {
    return Syscall3(static_cast<unsigned long long>(cinux::syscall::SyscallNr::kBrk), target, 0, 0);
}

}  // namespace user
