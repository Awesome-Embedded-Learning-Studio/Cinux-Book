/**
 * @file kernel/syscall/syscall_nums.hpp
 * @brief System call number constants
 *
 * Defines the syscall numbers used by user-space programs to request
 * kernel services via the SYSCALL instruction.  These numbers are
 * shared between kernel dispatch and the user-space libc wrapper.
 *
 * Convention: numbers match Linux x86_64 where practical to simplify
 * future porting of user programs.
 *
 * Namespace: cinux::syscall
 */

#pragma once

#include <stdint.h>

namespace cinux::syscall {

enum class SyscallNr : uint64_t {
    // --- low numbers (classic Unix, mostly stable across arches) ---
    SYS_read           = 0,
    SYS_write          = 1,
    SYS_open           = 2,
    SYS_close          = 3,
    SYS_stat           = 4,
    SYS_fstat          = 5,
    SYS_lseek          = 8,   ///< reposition read/write offset (musl fseek)
    SYS_mmap           = 9,   ///< allocate virtual memory (F2-M2)
    SYS_mprotect       = 10,  ///< set protection on a region (F2-M2)
    SYS_munmap         = 11,  ///< unmap virtual memory (F2-M2)
    SYS_brk            = 12,  ///< set program break / heap end (F2-M3)
    SYS_rt_sigaction   = 13,  ///< examine/set signal action (F3-M1)
    SYS_rt_sigprocmask = 14,  ///< examine/set signal mask (F3-M1)
    SYS_rt_sigreturn   = 15,  ///< return from signal handler (F3-M1, batch 3)
    SYS_ioctl          = 16,  ///< device-specific control (musl __stdout_write TIOCGWINSZ probe)
    SYS_readv          = 19,  ///< read into multiple buffers (musl __stdio_read)
    SYS_writev         = 20,  ///< write from multiple buffers (musl __stdio_write)
    SYS_pipe           = 22,
    SYS_yield          = 24,  ///< sched_yield
    SYS_getpid         = 39,
    SYS_fork           = 57,
    SYS_clone          = 56,  ///< create a thread/process (F3-M2)
    SYS_execve         = 59,
    SYS_exit           = 60,
    SYS_waitpid  = 61,  ///< Linux x86_64 slot 61 is wait4 (4-arg; musl waitpid passes rusage=NULL)
    SYS_kill     = 62,  ///< send a signal to a process (F3-M1)
    SYS_getdents = 78,
    SYS_getcwd   = 79,
    SYS_chdir    = 80,  ///< change working directory (was wrongly 12, collided with brk)
    SYS_mkdir    = 83,
    SYS_rmdir    = 84,
    SYS_creat    = 85,
    SYS_mknod    = 133,  ///< create a filesystem node (FIFO via S_IFIFO; F8-M2)
    SYS_unlink   = 87,
    SYS_getuid   = 102,  ///< get real user id (F9 M3)
    SYS_dmesg    = 103,  ///< kernel log read (Linux SYS_syslog)
    SYS_getgid   = 104,  ///< get real group id (F9 M3)
    SYS_setuid   = 105,  ///< set user id (F9 M3)
    SYS_setgid   = 106,  ///< set group id (F9 M3)
    SYS_geteuid  = 107,  ///< get effective user id (F9 M3)
    SYS_getegid  = 108,  ///< get effective group id (F9 M3)
    SYS_setpgid  = 109,  ///< set process-group id (F3-M3)
    SYS_getppid  = 110,
    SYS_setsid   = 112,  ///< create session + pgrp, become leader (F3-M3)
    SYS_getpgid  = 121,  ///< get process-group id (F3-M3)
    SYS_getsid   = 124,  ///< get session id (F3-M3)
    SYS_futex    = 202,  ///< fast user-space mutex (F3-M2)
    // --- musl-required numbers added in F10-M1 batch 4 ---
    SYS_arch_prctl      = 158,  ///< set/get thread FS/GS base (musl __init_tp ARCH_SET_FS for TLS)
    SYS_set_tid_address = 218,  ///< record cleartid addr; returns tid (musl __init_tp)
    SYS_clock_gettime   = 228,  ///< read a clock (musl time; 99 is sysinfo, not clock_gettime)
    SYS_exit_group      = 231,  ///< terminate thread group (musl exit(); falls back to SYS_exit)
    SYS_openat          = 257,  ///< open relative to dirfd (musl open/openat; AT_FDCWD=-100)
    SYS_newfstatat      = 262,  ///< stat relative to dirfd (musl stat/fstat/lstat)
    SYS_ping           = 220,  ///< ICMP echo (F7 shell ping; Cinux-custom)
};

/// Dispatch table covers all assigned Linux x86_64 numbers (max ~440) with
/// headroom; openat=257 / newfstatat=262 exceed the old size of 256.
constexpr uint64_t SYSCALL_TABLE_SIZE = 512;

}  // namespace cinux::syscall
