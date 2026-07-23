/**
 * @file kernel/arch/x86_64/syscall.cpp
 * @brief SYSCALL/SYSRET based system call infrastructure implementation
 *
 * Implements syscall_init() which configures the SYSCALL MSRs,
 * syscall_dispatch() which routes system calls to the appropriate handler,
 * and syscall_register() for handler registration.
 */

#include "kernel/arch/x86_64/syscall.hpp"

#include <stddef.h>
#include <stdint.h>

#include "kernel/arch/x86_64/gdt.hpp"
#include "kernel/errno.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/proc/signal.hpp"
#include "kernel/syscall/sys_arch_prctl.hpp"
#include "kernel/syscall/sys_brk.hpp"
#include "kernel/syscall/sys_chdir.hpp"
#include "kernel/syscall/sys_clock_gettime.hpp"
#include "kernel/syscall/sys_clone.hpp"
#include "kernel/syscall/sys_close.hpp"
#include "kernel/syscall/sys_creat.hpp"
#include "kernel/syscall/sys_creds.hpp"
#include "kernel/syscall/sys_dmesg.hpp"
#include "kernel/syscall/sys_execve.hpp"
#include "kernel/syscall/sys_exit.hpp"
#include "kernel/syscall/sys_fork.hpp"
#include "kernel/syscall/sys_futex.hpp"
#include "kernel/syscall/sys_getcwd.hpp"
#include "kernel/syscall/sys_getdents.hpp"
#include "kernel/syscall/sys_getpid.hpp"
#include "kernel/syscall/sys_getppid.hpp"
#include "kernel/syscall/sys_ioctl.hpp"
#include "kernel/syscall/sys_iov.hpp"
#include "kernel/syscall/sys_lseek.hpp"
#include "kernel/syscall/sys_mkdir.hpp"
#include "kernel/syscall/sys_mmap.hpp"
#include "kernel/syscall/sys_open.hpp"
#include "kernel/syscall/sys_pgrp.hpp"
#include "kernel/syscall/sys_ping.hpp"
#include "kernel/syscall/sys_pipe.hpp"
#include "kernel/syscall/sys_read.hpp"
#include "kernel/syscall/sys_rmdir.hpp"
#include "kernel/syscall/sys_set_tid_address.hpp"
#include "kernel/syscall/sys_signal.hpp"
#include "kernel/syscall/sys_stat.hpp"
#include "kernel/syscall/sys_unlink.hpp"
#include "kernel/syscall/sys_waitpid.hpp"
#include "kernel/syscall/sys_write.hpp"
#include "kernel/syscall/sys_yield.hpp"

namespace cinux::arch {

extern "C" void syscall_entry();

// ============================================================
// Internal state
// ============================================================

namespace {

using cinux::lib::kprintf;

SyscallFn syscall_table[cinux::syscall::SYSCALL_TABLE_SIZE] = {};

uint64_t g_syscall_kernel_rsp = 0;

void write_msr(uint32_t msr, uint64_t value) {
    __asm__ volatile("wrmsr"
                     :
                     : "c"(msr), "a"(static_cast<uint32_t>(value & 0xFFFFFFFF)),
                       "d"(static_cast<uint32_t>(value >> 32)));
}

/// Register all built-in syscall handlers into the dispatch table
void register_builtin_handlers() {
    using namespace cinux::syscall;
    syscall_register(SyscallNr::SYS_read, sys_read);
    syscall_register(SyscallNr::SYS_write, sys_write);
    syscall_register(SyscallNr::SYS_open, sys_open);
    syscall_register(SyscallNr::SYS_close, sys_close);
    syscall_register(SyscallNr::SYS_exit, sys_exit);
    syscall_register(SyscallNr::SYS_yield, sys_yield);
    syscall_register(SyscallNr::SYS_getdents, sys_getdents);
    syscall_register(SyscallNr::SYS_creat, sys_creat);
    syscall_register(SyscallNr::SYS_mkdir, sys_mkdir);
    syscall_register(SyscallNr::SYS_unlink, sys_unlink);
    syscall_register(SyscallNr::SYS_rmdir, sys_rmdir);
    syscall_register(SyscallNr::SYS_chdir, sys_chdir);
    syscall_register(SyscallNr::SYS_getcwd, sys_getcwd);
    syscall_register(SyscallNr::SYS_stat, sys_stat);
    syscall_register(SyscallNr::SYS_fstat, sys_fstat);
    syscall_register(SyscallNr::SYS_pipe, sys_pipe);
    syscall_register(SyscallNr::SYS_getpid, sys_getpid);
    syscall_register(SyscallNr::SYS_getppid, sys_getppid);
    syscall_register(SyscallNr::SYS_fork, sys_fork);
    syscall_register(SyscallNr::SYS_clone, sys_clone);
    syscall_register(SyscallNr::SYS_execve, sys_execve);
    syscall_register(SyscallNr::SYS_waitpid, sys_waitpid);
    syscall_register(SyscallNr::SYS_dmesg, sys_dmesg);
    syscall_register(SyscallNr::SYS_mmap, sys_mmap);
    syscall_register(SyscallNr::SYS_munmap, sys_munmap);
    syscall_register(SyscallNr::SYS_mprotect, sys_mprotect);
    syscall_register(SyscallNr::SYS_brk, sys_brk);
    syscall_register(SyscallNr::SYS_kill, sys_kill);
    syscall_register(SyscallNr::SYS_rt_sigaction, sys_rt_sigaction);
    syscall_register(SyscallNr::SYS_rt_sigprocmask, sys_rt_sigprocmask);
    syscall_register(SyscallNr::SYS_futex, sys_futex);
    syscall_register(SyscallNr::SYS_setpgid, sys_setpgid);
    syscall_register(SyscallNr::SYS_setsid, sys_setsid);
    syscall_register(SyscallNr::SYS_getpgid, sys_getpgid);
    syscall_register(SyscallNr::SYS_getsid, sys_getsid);
    syscall_register(SyscallNr::SYS_getuid, sys_getuid);
    syscall_register(SyscallNr::SYS_geteuid, sys_geteuid);
    syscall_register(SyscallNr::SYS_getgid, sys_getgid);
    syscall_register(SyscallNr::SYS_getegid, sys_getegid);
    syscall_register(SyscallNr::SYS_setuid, sys_setuid);
    syscall_register(SyscallNr::SYS_setgid, sys_setgid);

    // F10-M1 batch 4: musl-required syscalls (startup, vector I/O, *at, time).
    syscall_register(SyscallNr::SYS_lseek, sys_lseek);
    syscall_register(SyscallNr::SYS_ioctl, sys_ioctl);
    syscall_register(SyscallNr::SYS_readv, sys_readv);
    syscall_register(SyscallNr::SYS_writev, sys_writev);
    syscall_register(SyscallNr::SYS_arch_prctl, sys_arch_prctl);
    syscall_register(SyscallNr::SYS_set_tid_address, sys_set_tid_address);
    syscall_register(SyscallNr::SYS_clock_gettime, sys_clock_gettime);
    syscall_register(SyscallNr::SYS_exit_group, sys_exit_group);
    syscall_register(SyscallNr::SYS_openat, sys_openat);
    syscall_register(SyscallNr::SYS_newfstatat, sys_newfstatat);

    // F7: ICMP echo (shell ping).
    syscall_register(SyscallNr::SYS_ping, sys_ping);
}

}  // anonymous namespace

// ============================================================
// Public interface
// ============================================================

void syscall_init() {
    // Capture the current kernel stack pointer for syscall_entry
    uint64_t kernel_rsp;
    __asm__ volatile("movq %%rsp, %0" : "=r"(kernel_rsp));
    g_syscall_kernel_rsp = kernel_rsp;

    for (uint64_t i = 0; i < cinux::syscall::SYSCALL_TABLE_SIZE; i++) {
        syscall_table[i] = nullptr;
    }

    constexpr uint32_t MSR_STAR   = 0xC0000081;
    constexpr uint32_t MSR_LSTAR  = 0xC0000082;
    constexpr uint32_t MSR_SFMASK = 0xC0000084;

    uint64_t star_val = (static_cast<uint64_t>(GDT_SYSRET_BASE) << 48) |
                        (static_cast<uint64_t>(GDT_KERNEL_CODE) << 32);
    write_msr(MSR_STAR, star_val);

    uint64_t entry_addr = reinterpret_cast<uint64_t>(syscall_entry);
    write_msr(MSR_LSTAR, entry_addr);

    uint64_t sfmask_val = 0x200;
    write_msr(MSR_SFMASK, sfmask_val);

    // Register all built-in syscall handlers
    register_builtin_handlers();

    kprintf("[SYSCALL] LSTAR=%p STAR configured SFMASK=0x200 (clear IF)\n",
            reinterpret_cast<void*>(entry_addr));
}

void syscall_register(cinux::syscall::SyscallNr nr, SyscallFn handler) {
    uint64_t idx = static_cast<uint64_t>(nr);
    if (idx >= cinux::syscall::SYSCALL_TABLE_SIZE) {
        return;
    }
    syscall_table[idx] = handler;
}

uint64_t syscall_get_kernel_rsp() {
    return g_syscall_kernel_rsp;
}

}  // namespace cinux::arch

// ============================================================
// C-linkage dispatcher (called from syscall.S)
// ============================================================

extern "C" int64_t syscall_dispatch(uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                                    uint64_t a5, uint64_t a6) {
    if (nr >= cinux::syscall::SYSCALL_TABLE_SIZE) {
        cinux::lib::kprintf("[DISPATCH] invalid syscall nr=%u\n", static_cast<unsigned>(nr));
        return -cinux::kEnosys;
    }

    auto fn = cinux::arch::syscall_table[nr];
    if (fn == nullptr) {
        // Unregistered syscall: return -ENOSYS (not bare -1) so a probing
        // libc (musl probes rseq/prlimit/...) can fall back gracefully.
        // musl treats -4095..-1 as -errno, so bare -1 would read as EPERM.
        cinux::lib::kprintf("[SYSCALL] unhandled syscall %u\n", static_cast<unsigned>(nr));
        return -cinux::kEnosys;
    }

    int64_t ret = fn(a1, a2, a3, a4, a5, a6);
    // F3-M1 batch 2: deliver one pending signal before returning to user.
    cinux::proc::signal_check_and_deliver();
    return ret;
}
