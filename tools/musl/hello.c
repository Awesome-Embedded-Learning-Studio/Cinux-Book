/*
 * hello.c — minimal musl static program for Cinux (F10-M1 batch 5/6).
 *
 * Linked against the self-contained musl sysroot (see build-musl.sh).  This is
 * the smoke binary for the F10-M1 user-runtime arc: batch 5 produces it on the
 * host, batch 6 loads it via execve + the ELF loader + the batch-3 initial
 * stack and runs it under QEMU.
 *
 * Uses write() (a direct syscall) rather than printf(): the full musl runtime
 * -- _start, __init_libc, __init_tls (arch_prctl/ARCH_SET_FS), stack canary
 * (%fs:0x28), __libc_start_init, exit_group -- runs end-to-end under Cinux,
 * and write() exercises the batch-4 SYS_write path directly.  printf()'s stdio
 * path (the lazy stdout FILE init in __stdout_write) hits a separate segfault
 * that is tracked as a follow-up; it does not affect the runtime proof.
 */
#include <unistd.h>

int main(void) {
    write(1, "Hello from musl on Cinux!\n", 26);
    return 0;
}
