#include "kernel/syscall/syscall.hpp"

#include <array>

#include "kernel/arch/x86_64/halt.hpp"
#include "kernel/console/console.hpp"
#include "kernel/driver/keyboard.hpp"
#include "kernel/proc/scheduler.hpp"

namespace {

using cinux::syscall::SyscallNr;
using Handler = long long (*)(unsigned long long, unsigned long long, unsigned long long);

long long handle_read(unsigned long long descriptor, unsigned long long buffer,
                      unsigned long long count) {
    if (descriptor != 0) {
        return -cinux::syscall::kEbadf;
    }
    if (buffer >= cinux::syscall::kUserAddressLimit) {
        return -cinux::syscall::kEfault;
    }
    auto* out = reinterpret_cast<char*>(buffer);  // NOLINT(performance-no-int-to-ptr)
    for (unsigned long long got = 0; got < count; ++got) {
        char glyph = cinux::driver::Keyboard::self().take(true);
        if (glyph == '\r') {
            glyph = '\n';
        }
        out[got] = glyph;
        if (glyph == '\n') {
            return static_cast<long long>(got + 1);
        }
    }
    return static_cast<long long>(count);
}

long long handle_write(unsigned long long descriptor, unsigned long long buffer,
                       unsigned long long count) {
    if (descriptor != 1) {
        return -cinux::syscall::kEbadf;
    }
    if (buffer >= cinux::syscall::kUserAddressLimit) {
        return -cinux::syscall::kEfault;
    }
    auto const* text = reinterpret_cast<const char*>(buffer);  // NOLINT(performance-no-int-to-ptr)
    for (unsigned long long i = 0; i < count; ++i) {
        cinux::console::PutChar(text[i]);
    }
    return static_cast<long long>(count);
}

long long handle_yield([[maybe_unused]] unsigned long long first,
                       [[maybe_unused]] unsigned long long second,
                       [[maybe_unused]] unsigned long long third) {
    cinux::proc::Scheduler::self().yield();
    return 0;
}

[[noreturn]] long long handle_exit([[maybe_unused]] unsigned long long first,
                                   [[maybe_unused]] unsigned long long second,
                                   [[maybe_unused]] unsigned long long third) {
    cinux::proc::Scheduler::self().exit_current();
    cinux::arch::Halt();
}

constexpr std::array<Handler, cinux::syscall::kSyscallCount> make_handler_table() {
    std::array<Handler, cinux::syscall::kSyscallCount> table{};
    table[static_cast<unsigned long long>(SyscallNr::kRead)]  = handle_read;
    table[static_cast<unsigned long long>(SyscallNr::kWrite)] = handle_write;
    table[static_cast<unsigned long long>(SyscallNr::kYield)] = handle_yield;
    table[static_cast<unsigned long long>(SyscallNr::kExit)]  = handle_exit;
    return table;
}

constexpr std::array<Handler, cinux::syscall::kSyscallCount> kHandlers = make_handler_table();

}  // namespace

long long InvokeSyscall(unsigned long long number, unsigned long long arg1, unsigned long long arg2,
                        unsigned long long arg3) {
    if (number >= cinux::syscall::kSyscallCount || kHandlers[number] == nullptr) {
        return -cinux::syscall::kEnosys;
    }
    return kHandlers[number](arg1, arg2, arg3);
}
