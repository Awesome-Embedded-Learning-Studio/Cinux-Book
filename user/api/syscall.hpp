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
 * @date    2026-10-07
 * @version 0.1
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
inline long long Read(unsigned long long descriptor, void* buffer, unsigned long long count) {
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
inline long long Getdents(unsigned long long descriptor, cinux::syscall::SyscallDirent* entry) {
    return Syscall3(static_cast<unsigned long long>(cinux::syscall::SyscallNr::kGetdents),
                    descriptor, reinterpret_cast<unsigned long long>(entry), 0);
}

/// exit(code): end this task; never returns.
[[noreturn]] inline void Exit(unsigned long long code) {
    static_cast<void>(
        Syscall3(static_cast<unsigned long long>(cinux::syscall::SyscallNr::kExit), code, 0, 0));
    __builtin_unreachable();
}

}  // namespace user
