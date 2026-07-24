/**
 * @file kernel/syscall/sys_cinux_exit.cpp
 * @brief sys_cinux_exit handler -- write isa-debug-exit (port 0xf4).
 *
 * A single outl to the isa-debug-exit device terminates QEMU with
 * (code<<1)|1. Mirrors what the test harness (test/main_test.cpp) and the
 * panic path (exception_handlers.cpp) already do from kernel context; this
 * handler just exposes the same primitive to ring 3 for the buildroot-
 * usability CI gate (busybox init -> inittab ::once -> test script ->
 * cinux-exit helper -> this syscall).
 */

#include "kernel/syscall/sys_cinux_exit.hpp"

#include <stdint.h>

#include "kernel/arch/x86_64/io.hpp"

namespace cinux::syscall {

/// QEMU isa-debug-exit iobase (cmake/qemu.cmake: -device isa-debug-exit,iobase=0xf4).
constexpr uint16_t kQemuExitPort = 0xf4;

int64_t sys_cinux_exit(uint64_t code, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
    // Never returns: QEMU exits with (code<<1)|1. Truncated to uint32_t to
    // match the device's iosize=4 (and the outl width the test harness uses).
    cinux::io::io_outl(kQemuExitPort, static_cast<uint32_t>(code));
    return 0;  // unreachable -- QEMU has exited
}

}  // namespace cinux::syscall
