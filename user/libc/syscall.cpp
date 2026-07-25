/**
 * @file user/libc/syscall.c
 * @brief User-space system call wrapper implementations
 *
 * Each wrapper loads the syscall number from the shared syscall_nums
 * constants and executes the SYSCALL instruction with the appropriate
 * arguments.
 */

#include "syscall.h"

#include "kernel/syscall/syscall_nums.hpp"

static inline int64_t _syscall1(uint64_t nr, uint64_t a1) {
    int64_t ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(nr), "D"(a1) : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t _syscall2(uint64_t nr, uint64_t a1, uint64_t a2) {
    int64_t ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(nr), "D"(a1), "S"(a2) : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t _syscall3(uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3) {
    int64_t ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t _syscall5(uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                                uint64_t a5) {
    int64_t           ret;
    register uint64_t r4 __asm__("r10") = a4;
    register uint64_t r5 __asm__("r8")  = a5;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr), "D"(a1), "S"(a2), "d"(a3), "r"(r4), "r"(r5)
                     : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t _syscall6(uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                                uint64_t a5, uint64_t a6) {
    int64_t           ret;
    register uint64_t r4 __asm__("r10") = a4;
    register uint64_t r5 __asm__("r8")  = a5;
    register uint64_t r6 __asm__("r9")  = a6;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr), "D"(a1), "S"(a2), "d"(a3), "r"(r4), "r"(r5), "r"(r6)
                     : "rcx", "r11", "memory");
    return ret;
}

using cinux::syscall::SyscallNr;

int64_t sys_open(const char* path, int flags) {
    return _syscall2(static_cast<uint64_t>(SyscallNr::SYS_open), (uint64_t)path, (uint64_t)flags);
}

int64_t sys_close(int fd) {
    return _syscall1(static_cast<uint64_t>(SyscallNr::SYS_close), (uint64_t)fd);
}

int64_t sys_read(int fd, void* buf, size_t count) {
    return _syscall3(static_cast<uint64_t>(SyscallNr::SYS_read), (uint64_t)fd, (uint64_t)buf,
                     (uint64_t)count);
}

int64_t sys_write(int fd, const void* buf, size_t count) {
    return _syscall3(static_cast<uint64_t>(SyscallNr::SYS_write), (uint64_t)fd, (uint64_t)buf,
                     (uint64_t)count);
}

int64_t sys_getdents(int fd, void* buf, size_t count) {
    return _syscall3(static_cast<uint64_t>(SyscallNr::SYS_getdents), (uint64_t)fd, (uint64_t)buf,
                     (uint64_t)count);
}

int64_t sys_creat(const char* path) {
    return _syscall1(static_cast<uint64_t>(SyscallNr::SYS_creat), (uint64_t)path);
}

int64_t sys_mknod(const char* path, uint32_t mode, uint32_t dev) {
    return _syscall3(static_cast<uint64_t>(SyscallNr::SYS_mknod), (uint64_t)path, (uint64_t)mode,
                     (uint64_t)dev);
}

int64_t sys_mkdir(const char* path) {
    return _syscall1(static_cast<uint64_t>(SyscallNr::SYS_mkdir), (uint64_t)path);
}

int64_t sys_unlink(const char* path) {
    return _syscall1(static_cast<uint64_t>(SyscallNr::SYS_unlink), (uint64_t)path);
}

int64_t sys_rmdir(const char* path) {
    return _syscall1(static_cast<uint64_t>(SyscallNr::SYS_rmdir), (uint64_t)path);
}

int64_t sys_chdir(const char* path) {
    return _syscall1(static_cast<uint64_t>(SyscallNr::SYS_chdir), (uint64_t)path);
}

int64_t sys_getcwd(char* buf, size_t size) {
    return _syscall2(static_cast<uint64_t>(SyscallNr::SYS_getcwd), (uint64_t)buf, (uint64_t)size);
}

int64_t sys_stat(const char* path, struct sys_stat* st) {
    return _syscall2(static_cast<uint64_t>(SyscallNr::SYS_stat), (uint64_t)path, (uint64_t)st);
}

int64_t sys_fstat(int fd, struct sys_stat* st) {
    return _syscall2(static_cast<uint64_t>(SyscallNr::SYS_fstat), (uint64_t)fd, (uint64_t)st);
}

int64_t sys_ping(uint32_t ip_packed, uint16_t id, uint16_t seq) {
    return _syscall3(static_cast<uint64_t>(SyscallNr::SYS_ping), (uint64_t)ip_packed, (uint64_t)id,
                     (uint64_t)seq);
}

void sys_exit(int code) {
    _syscall1(static_cast<uint64_t>(SyscallNr::SYS_exit), (uint64_t)code);
    __builtin_unreachable();
}

void sys_yield(void) {
    _syscall1(static_cast<uint64_t>(SyscallNr::SYS_yield), 0);
}

int64_t sys_fork(void) {
    return _syscall1(static_cast<uint64_t>(SyscallNr::SYS_fork), 0);
}

int64_t sys_execve(const char* path, char* const argv[], char* const envp[]) {
    return _syscall3(static_cast<uint64_t>(SyscallNr::SYS_execve), (uint64_t)path, (uint64_t)argv,
                     (uint64_t)envp);
}

int64_t sys_waitpid(int pid, int* status, int options) {
    return _syscall3(static_cast<uint64_t>(SyscallNr::SYS_waitpid), (uint64_t)pid, (uint64_t)status,
                     (uint64_t)options);
}

int64_t sys_kill(int pid, int sig) {
    return _syscall2(static_cast<uint64_t>(SyscallNr::SYS_kill), (uint64_t)pid, (uint64_t)sig);
}

int64_t sys_sigaction(int sig, const struct sys_sigaction* act, struct sys_sigaction* old) {
    return _syscall3(static_cast<uint64_t>(SyscallNr::SYS_rt_sigaction), (uint64_t)sig,
                     (uint64_t)act, (uint64_t)old);
}

int64_t sys_setpgid(int pid, int pgid) {
    return _syscall2(static_cast<uint64_t>(SyscallNr::SYS_setpgid), (uint64_t)pid, (uint64_t)pgid);
}

int64_t sys_setsid(void) {
    return _syscall1(static_cast<uint64_t>(SyscallNr::SYS_setsid), 0);
}

int64_t sys_getpgid(int pid) {
    return _syscall1(static_cast<uint64_t>(SyscallNr::SYS_getpgid), (uint64_t)pid);
}

int64_t sys_getsid(int pid) {
    return _syscall1(static_cast<uint64_t>(SyscallNr::SYS_getsid), (uint64_t)pid);
}

// Process credentials (F9 batch 9 / M3).
int64_t sys_getuid(void) {
    return _syscall1(static_cast<uint64_t>(SyscallNr::SYS_getuid), 0);
}

int64_t sys_geteuid(void) {
    return _syscall1(static_cast<uint64_t>(SyscallNr::SYS_geteuid), 0);
}

int64_t sys_getgid(void) {
    return _syscall1(static_cast<uint64_t>(SyscallNr::SYS_getgid), 0);
}

int64_t sys_getegid(void) {
    return _syscall1(static_cast<uint64_t>(SyscallNr::SYS_getegid), 0);
}

int64_t sys_setuid(uint32_t uid) {
    return _syscall1(static_cast<uint64_t>(SyscallNr::SYS_setuid), (uint64_t)uid);
}

int64_t sys_setgid(uint32_t gid) {
    return _syscall1(static_cast<uint64_t>(SyscallNr::SYS_setgid), (uint64_t)gid);
}

int64_t sys_sigprocmask(int how, const uint64_t* set, uint64_t* old) {
    return _syscall3(static_cast<uint64_t>(SyscallNr::SYS_rt_sigprocmask), (uint64_t)how,
                     (uint64_t)set, (uint64_t)old);
}

// ============================================================
// Thread support (F3-M2 batch 5)
// ============================================================

int64_t sys_clone(uint64_t flags, void* stack, int* parent_tid, int* child_tid, void* tls) {
    return _syscall5(static_cast<uint64_t>(SyscallNr::SYS_clone), flags, (uint64_t)stack,
                     (uint64_t)parent_tid, (uint64_t)child_tid, (uint64_t)tls);
}

int64_t sys_futex(uint32_t* uaddr, int op, uint32_t val, uint32_t val3) {
    // timeout and uaddr2 are unsupported (no timeout/requeue); pass 0.
    return _syscall6(static_cast<uint64_t>(SyscallNr::SYS_futex), (uint64_t)uaddr, (uint64_t)op,
                     (uint64_t)val, 0, 0, (uint64_t)val3);
}
