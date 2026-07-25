/*
 * cinux-exit.c -- trigger the Cinux sys_cinux_exit syscall (221) to terminate
 * the QEMU run with a pass/fail code the CI gate reads via isa-debug-exit.
 *
 * The Cinux kernel has no iopl/ioperm and sys_reboot is a -EPERM stub, so this
 * (calling SYS_cinux_exit from the buildroot-usability test script) is the only
 * userspace -> QEMU-exit path. The kernel handler writes port 0xf4; QEMU exits
 * with (code<<1)|1 (0 -> exit 1 = success, non-zero -> exit 3+ = failure).
 *
 * Built statically against the Cinux musl sysroot (tools/musl/build-cinux-exit.sh)
 * and placed in rootfs/overlay/sbin/ so Buildroot packs it into rootfs.ext2.
 *
 * Usage: cinux-exit [code]   (default 0 = success)
 */

#include <stdlib.h>
#include <unistd.h>

/* Cinux-custom syscall; not in musl's sys/syscall.h. Keep in sync with
 * SYS_cinux_exit in kernel/syscall/syscall_nums.hpp. */
#define SYS_cinux_exit 221

int main(int argc, char **argv) {
    /* atoi, not strtol: GCC>=14 redirects strtol to __isoc23_strtol, which musl
     * libc.a does not provide (link failure). atoi's semantics are unchanged in
     * C23 so it is not redirected. */
    long code = (argc > 1) ? atoi(argv[1]) : 0;
    syscall(SYS_cinux_exit, code);
    return 0; /* unreachable -- QEMU exits inside the syscall */
}
