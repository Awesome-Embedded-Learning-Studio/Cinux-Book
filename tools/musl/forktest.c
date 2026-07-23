/*
 * forktest.c -- SMP CoW race reproducer for CinuxOS (F10 follow-up).
 *
 * A musl static user program that forks repeatedly via the raw SYS_fork (57)
 * syscall -- the exact path the shell's launch_program uses.  After each fork
 * the parent writes a shared CoW page; the child reads it.  Under correct CoW
 * the child must see the PRE-fork value (the parent's post-fork write is
 * isolated to the parent's private copy).  If the kernel forgot to flush the
 * parent TLB after CoW-marking its page table, the parent writes THROUGH the
 * stale writable TLB entry to the shared physical frame and the child observes
 * the leaked value -- a detected RACE.  A crash (Double Fault / segfault from a
 * corrupted shared stack) also counts as failure.
 *
 * Run under -smp 2 (KVM) by loading this binary as /hello so the F10-M1 ring-3
 * smoke launches it.  The smoke worker forks a child that execve's /hello; that
 * child is a real user task with an address space, so ITS forks exercise CoW.
 *
 * Output: "FORKTEST iters=N races=M clean=K errs=E\n" to stdout (serial).
 * Exit 0 if races==0 && errs==0, else 1.
 *
 * Uses only raw syscalls (write/fork/wait4/yield/exit_group) -- no printf, whose
 * musl stdout-FILE path is a separate known segfault on CinuxOS today.
 */
#include <unistd.h>

static long sys_write(int fd, const void* buf, unsigned long n) {
    long ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(1L), "D"((long)fd), "S"(buf), "d"(n)
                     : "rcx", "r11", "memory");
    return ret;
}

static long sys_fork(void) {
    long ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(57L) : "rcx", "r11", "memory");
    return ret;
}

/* SYS_wait4 (61): wait4(pid, status, options, rusage=NULL). */
static long sys_wait4(int pid, int* status, int options) {
    long          ret;
    register long r10 __asm__("r10") = 0; /* rusage = NULL */
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(61L), "D"((long)pid), "S"((long)status), "d"((long)options), "r"(r10)
                     : "rcx", "r11", "memory");
    return ret;
}

static void sys_yield(void) {
    __asm__ volatile("syscall" : : "a"(24L), "D"(0L) : "rcx", "r11", "memory");
}

static void sys_exit_group(int code) {
    __asm__ volatile("syscall" : : "a"(231L), "D"((long)code));
    __builtin_unreachable();
}

/* Shared marker in a writable page -- CoW'd at fork.  volatile so the load/store
 * are actually emitted (not cached in a register across the syscall). */
static volatile long g_marker;

/* Append "name=val " to buf at *off. */
static void put_kv(char* buf, int* off, const char* name, long val) {
    for (const char* p = name; *p; p++)
        buf[(*off)++] = *p;
    buf[(*off)++] = '=';
    char tmp[24];
    int  n = 0;
    if (val == 0)
        tmp[n++] = '0';
    for (long v = val; v > 0; v /= 10)
        tmp[n++] = (char)('0' + (v % 10));
    while (n > 0)
        buf[(*off)++] = tmp[--n];
    buf[(*off)++] = ' ';
}

int main(void) {
#ifndef FORKTEST_ITERS
#    define FORKTEST_ITERS 300
#endif
    const long ITERS = FORKTEST_ITERS;
    long       races = 0, clean = 0, errs = 0;

    for (long i = 1; i <= ITERS; i++) {
        long baseline = 0x1000 + i;
        g_marker      = baseline; /* pre-fork value both sides start with */

        long pid = sys_fork();
        if (pid < 0) {
            errs++;
            continue;
        }
        if (pid == 0) {
            /* Child: read the shared CoW page.  Must see the baseline -- the
             * parent's post-fork write is isolated to its private copy.  A
             * leaked parent write (stale writable TLB) => observed != baseline
             * => RACE. */
            long v = g_marker;
            sys_exit_group(v == baseline ? 0 : 1);
        }

        /* Parent: write a NEW value to the shared page IMMEDIATELY after fork.
         * If the parent TLB still caches the old WRITABLE entry (the bug), this
         * write lands on the shared physical frame and the child reads it. */
#ifndef FORKTEST_NO_PARENT_WRITE
        g_marker = 0xdead + i;
#endif

        /* Reap the child (WNOHANG poll + yield -- blocking wait is not reliable
         * under the smoke's minimal scheduler; -smp 2 lets the child run on the
         * other CPU while we poll). */
        int  st = 0;
        long rr = 0;
        for (int spins = 0; spins < 2'000'000; spins++) {
            rr = sys_wait4((int)pid, &st, 1 /*WNOHANG*/);
            if (rr != 0)
                break;
            sys_yield();
        }
        if (rr > 0) {
            if (st == 0)
                clean++; /* child saw baseline -- correct CoW */
            else
                races++; /* child saw the leaked parent write -- RACE */
        } else {
            errs++; /* reap failed / timed out */
        }
    }

    char buf[128];
    int  off = 0;
    put_kv(buf, &off, "FORKTEST iters", ITERS);
    put_kv(buf, &off, "races", races);
    put_kv(buf, &off, "clean", clean);
    put_kv(buf, &off, "errs", errs);
    buf[off++] = '\n';
    sys_write(1, buf, off);

    sys_exit_group((races == 0 && errs == 0) ? 0 : 1);
}
