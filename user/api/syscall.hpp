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
 * @brief         getdents(fd, entry): read one directory entry.
 * @param[in]     descriptor   An open directory.
 * @param[out]    entry        Filled with the next name and its type.
 * @return        One when filled, zero at the end, or a negative errno.
 * @since         0.1.0
 */
inline long long Getdents(unsigned long long                   descriptor,
                          const cinux::syscall::SyscallDirent* entry) {
    return Syscall3(static_cast<unsigned long long>(cinux::syscall::SyscallNr::kGetdents),
                    descriptor, reinterpret_cast<unsigned long long>(entry), 0);
}

/**
 * @brief         stat(path, out): one node's metadata.
 * @param[in]     path   The path, relative or absolute.
 * @param[out]    out    Receives the node's metadata.
 * @return        Zero, or a negative errno.
 * @since         0.2.0
 */
inline long long Stat(const char* path, const cinux::syscall::SyscallStat* out) {
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
 * @brief         getpid(): the caller's own identity.
 * @return        The pid the scheduler handed this task.
 * @since         0.3.0
 */
inline long long Getpid() {
    return Syscall3(static_cast<unsigned long long>(cinux::syscall::SyscallNr::kGetpid), 0, 0, 0);
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
 * @brief         execve(path): replace this world with a program.
 * @param[in]     path   The program's absolute path on the volume.
 * @return        Never on success, or a negative errno.
 * @since         0.3.0
 */
[[noreturn]] inline void Execve(const char* path) {
    static_cast<void>(Syscall3(static_cast<unsigned long long>(cinux::syscall::SyscallNr::kExecve),
                               reinterpret_cast<unsigned long long>(path), 0, 0));
    __builtin_unreachable();
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
