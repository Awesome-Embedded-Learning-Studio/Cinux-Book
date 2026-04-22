/**
 * @file kernel/syscall/sys_read.cpp
 * @brief sys_read handler implementation
 *
 * Reads keyboard input from the PS/2 ring buffer into a user-space buffer.
 * Spins briefly waiting for input if the buffer is empty, converting
 * carriage returns to newlines for shell compatibility.
 */

#include "kernel/syscall/sys_read.hpp"

#include <stdint.h>

#include "kernel/drivers/keyboard/keyboard.hpp"

namespace cinux::syscall {

namespace {

constexpr uint64_t USER_ADDR_MAX = 0x800000000000ULL;

/// Maximum iterations to spin-wait for a key before returning 0
constexpr uint32_t SPIN_WAIT_ITERS = 1'000'000;

}  // anonymous namespace

int64_t sys_read(uint64_t fd, uint64_t buf_virt, uint64_t count,
                 uint64_t, uint64_t, uint64_t) {
    if (buf_virt >= USER_ADDR_MAX) {
        return -1;
    }

    if (fd != 0) {
        return -1;
    }

    auto* buf = reinterpret_cast<char*>(buf_virt);
    uint64_t read_bytes = 0;

    while (read_bytes < count) {
        cinux::drivers::KeyEvent ev;

        // Try to dequeue a key event; spin-wait if none available yet
        if (!cinux::drivers::Keyboard::poll(ev)) {
            if (read_bytes > 0) {
                // Already have some data -- return it immediately
                break;
            }

            // Spin-wait for the first character (yield via pause)
            bool got_key = false;
            for (uint32_t i = 0; i < SPIN_WAIT_ITERS; i++) {
                __asm__ volatile("pause");
                if (cinux::drivers::Keyboard::poll(ev)) {
                    got_key = true;
                    break;
                }
            }

            if (!got_key) {
                // No input available after waiting
                break;
            }
        }

        // Only accept pressed keys with printable ASCII
        if (!ev.pressed || ev.ascii == 0) {
            continue;
        }

        // Convert carriage return to newline for shell compatibility
        char ch = (ev.ascii == '\r') ? '\n' : ev.ascii;

        buf[read_bytes] = ch;
        read_bytes++;

        // Stop at newline so the shell gets one line at a time
        if (ch == '\n') {
            break;
        }
    }

    return static_cast<int64_t>(read_bytes);
}

}  // namespace cinux::syscall
