/**
 * @file user/libc/syscall.h
 * @brief User-space system call wrappers
 *
 * Provides C function wrappers for the SYSCALL instruction.
 * Syscall numbers are shared with the kernel via syscall_nums.hpp.
 *
 * Syscall convention (Linux x86_64):
 *   RAX = syscall number
 *   RDI = arg1, RSI = arg2, RDX = arg3
 *   R10 = arg4, R8  = arg5, R9  = arg6
 *   RAX = return value
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

int64_t sys_open(const char* path, int flags);
int64_t sys_close(int fd);
int64_t sys_read(int fd, void* buf, size_t count);
int64_t sys_write(int fd, const void* buf, size_t count);
int64_t sys_getdents(int fd, void* buf, size_t count);
int64_t sys_creat(const char* path);
int64_t sys_mkdir(const char* path);
int64_t sys_unlink(const char* path);
int64_t sys_rmdir(const char* path);
int64_t sys_chdir(const char* path);
int64_t sys_getcwd(char* buf, size_t size);
void    sys_exit(int code);
void    sys_yield(void);

/// Process management -- lets the shell fork + execve + reap a child program
/// (e.g. the musl static /hello).  These are the only user/libc wrappers the
/// shell launcher needs; musl programs bring their own libc.
int64_t sys_fork(void);
int64_t sys_execve(const char* path, char* const argv[], char* const envp[]);
int64_t sys_waitpid(int pid, int* status, int options);

/// Layout matches the Linux x86_64 `struct stat` (uapi/asm/stat.h), 144 bytes,
/// so musl/glibc binaries share the on-stack buffer with the kernel verbatim.
struct sys_stat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t __pad0;
    uint64_t st_rdev;
    int64_t  st_size;
    int64_t  st_blksize;
    int64_t  st_blocks;
    uint64_t st_atime;
    uint64_t st_atime_nsec;
    uint64_t st_mtime;
    uint64_t st_mtime_nsec;
    uint64_t st_ctime;
    uint64_t st_ctime_nsec;
    int64_t  __unused[3];
};

int64_t sys_stat(const char* path, struct sys_stat* st);
int64_t sys_fstat(int fd, struct sys_stat* st);

// ============================================================
// Signal support (F3-M1)
// ============================================================

// Core POSIX signal numbers.
#define SIGHUP  1
#define SIGINT  2
#define SIGQUIT 3
#define SIGILL  4
#define SIGTRAP 5
#define SIGABRT 6
#define SIGBUS  7
#define SIGFPE  8
#define SIGKILL 9
#define SIGUSR1 10
#define SIGSEGV 11
#define SIGUSR2 12
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM 15
#define SIGCHLD 17
#define SIGCONT 18
#define SIGSTOP 19
#define SIGTSTP 20

#define SIG_DFL     ((uint64_t)0)
#define SIG_IGN     ((uint64_t)1)
#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

// User-space sigaction layout, matching the Linux x86_64 `struct sigaction`
// (uapi/asm-generic/signal.h): { sa_handler, sa_flags, sa_restorer, sa_mask }.
// musl's k_sigaction is built in exactly this order and passed to rt_sigaction
// with sigsetsize = _NSIG/8 = 8.
struct sys_sigaction {
    uint64_t sa_handler;   ///< SIG_DFL (0) / SIG_IGN (1) / handler address
    uint64_t sa_flags;     ///< SA_* flags (SA_RESTART etc.)
    uint64_t sa_restorer;  ///< reserved (kernel injects its own sigreturn trampoline)
    uint64_t sa_mask;      ///< signals blocked during the handler (8-byte kernel sigset)
};

int64_t sys_kill(int pid, int sig);
int64_t sys_sigaction(int sig, const struct sys_sigaction* act, struct sys_sigaction* old);
int64_t sys_sigprocmask(int how, const uint64_t* set, uint64_t* old);

// ============================================================
// Thread support (F3-M2 batch 5)
// ============================================================

// Linux clone() flags.
#define CLONE_VM             0x00000100
#define CLONE_FS             0x00000200
#define CLONE_FILES          0x00000400
#define CLONE_SIGHAND        0x00000800
#define CLONE_THREAD         0x00010000
#define CLONE_SETTLS         0x00080000
#define CLONE_PARENT_SETTID  0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000
#define CLONE_CHILD_SETTID   0x01000000

// futex operations.
#define FUTEX_WAIT        0
#define FUTEX_WAKE        1
#define FUTEX_WAIT_BITSET 9
#define FUTEX_WAKE_BITSET 10

/// Create a thread/process sharing resources per @p flags.  Returns child tid
/// to the parent, 0 to the child, or -errno on failure.
int64_t sys_clone(uint64_t flags, void* stack, int* parent_tid, int* child_tid, void* tls);

/// Fast user-space mutex.  WAIT: block if *uaddr == val; WAKE: wake <= val
/// waiters.  Returns 0 / #woken / -errno.
int64_t sys_futex(uint32_t* uaddr, int op, uint32_t val, uint32_t val3);

// ============================================================
// Process-group / session control (F3-M3 batch 3)
// ============================================================

/// setpgid(pid, pgid): pid 0 => caller; pgid 0 => lead a new group (id == pid).
/// Returns 0 / -errno.
int64_t sys_setpgid(int pid, int pgid);

/// setsid(): caller founds a new session + process group.  Returns new sid / -errno.
int64_t sys_setsid(void);

/// getpgid(pid): pid 0 => caller.  Returns the group id / -errno.
int64_t sys_getpgid(int pid);

/// getsid(pid): pid 0 => caller.  Returns the session id / -errno.
int64_t sys_getsid(int pid);

// ============================================================
// Process credentials (F9 batch 9 / M3)
// ============================================================

/// getuid/geteuid/getgid/getegid: return the caller's real/effective IDs.
int64_t sys_getuid(void);
int64_t sys_geteuid(void);
int64_t sys_getgid(void);
int64_t sys_getegid(void);

/// setuid/setgid: set the effective ID. Root may set anything; a non-root task
/// may only drop back to its real ID. Returns 0 / -errno (EPERM).
int64_t sys_setuid(uint32_t uid);
int64_t sys_setgid(uint32_t gid);

// ============================================================
// Network (F7 shell ping)
// ============================================================

/// ping: send one ICMP echo to @p ip_packed (a.b.c.d MSB-first:
/// (a<<24)|(b<<16)|(c<<8)|d) and wait for the reply.  Returns 0 on reply,
/// -errno otherwise (ETIMEDOUT = no reply, ENOSYS = stack not up).
int64_t sys_ping(uint32_t ip_packed, uint16_t id, uint16_t seq);
