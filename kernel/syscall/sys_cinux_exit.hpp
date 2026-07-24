/**
 * @file kernel/syscall/sys_cinux_exit.hpp
 * @brief sys_cinux_exit handler declaration -- QEMU isa-debug-exit gate.
 *
 * Cinux-custom syscall (SYS_cinux_exit = 221). Lets a Buildroot userland
 * (ash + a test script) terminate the QEMU run with a pass/fail code that CI
 * gates on: the kernel writes port 0xf4 (isa-debug-exit, iosize=4) with the
 * code, and QEMU exits with (code<<1)|1 -- 0 -> exit 1 = success (qemu_test_
 * wrapper.sh maps 1 -> SUCCESS), non-zero -> exit 3+ = failure.
 *
 * Userspace cannot outb directly (no iopl/ioperm), and sys_reboot is a -EPERM
 * stub, so this syscall is the ONLY userspace -> isa-debug-exit path. It is
 * the test-harness + panic-path primitive (test/main_test.cpp + exception_
 * handlers.cpp do the same outl from kernel context) exposed to ring 3.
 *
 * Namespace: cinux::syscall
 */

#pragma once

#include <stdint.h>

#include "kernel/arch/x86_64/syscall.hpp"

namespace cinux::syscall {

/**
 * @brief Signal QEMU isa-debug-exit with a status code (never returns).
 *
 * @param code  0 = success (QEMU exits 1), non-zero = failure (QEMU exits 3+).
 * @return      Declared int64_t to match the syscall ABI; the outl traps to
 *              QEMU before the return, so this never actually returns.
 */
int64_t sys_cinux_exit(uint64_t code, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

}  // namespace cinux::syscall
