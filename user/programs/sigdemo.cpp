#include <api/syscall.hpp>

#include "kernel/syscall/signal.hpp"

namespace {

void put(const char* text) {
    unsigned long long length = 0;
    while (text[length] != '\0') {
        length++;
    }
    user::Write(1, text, length);
}

void put_number(unsigned long long value) {
    char               digits[24] = {};
    unsigned long long count      = 0;
    if (value == 0) {
        put("0");
        return;
    }
    while (value != 0) {
        digits[count] = static_cast<char>('0' + (value % 10ULL));
        count++;
        value /= 10ULL;
    }
    char out[24] = {};
    for (unsigned long long index = 0; index < count; ++index) {
        out[index] = digits[count - 1 - index];
    }
    user::Write(1, out, count);
}

void spin(unsigned long long rounds) {
    volatile unsigned long long sink = 0;
    for (unsigned long long index = 0; index < rounds; ++index) {
        sink = sink + 1ULL;
    }
}

[[noreturn]] void child_loop() {
    unsigned long long tick = 0;
    for (;;) {
        tick++;
        put("  [child] tick ");
        put_number(tick);
        put("\n");
        spin(20000000ULL);
    }
}

}  // namespace

// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" void _start() {
    const long long kPid = user::Fork();
    if (kPid == 0) {
        child_loop();
    }
    const auto kChild = static_cast<unsigned long long>(kPid);
    put("[parent] child lives as pid ");
    put_number(kChild);
    put("\n");

    user::Setpgid(kChild, kChild);
    put("[parent] child now leads group ");
    put_number(static_cast<unsigned long long>(user::Getpgid(kChild)));
    put("\n");

    put("[parent] kill(child, 0) probe answers ");
    put_number(static_cast<unsigned long long>(user::Kill(kChild, 0)));
    put("\n");

    put("[parent] SIGSTOP goes out; the ticks should fall silent\n");
    user::Kill(kChild, cinux::syscall::kSigStop);
    spin(600000000ULL);

    int             status = 0;
    const long long kProbe = user::Wait4NoHang(-1, &status);
    put("[parent] WNOHANG wait answers ");
    put_number(static_cast<unsigned long long>(kProbe));
    put(" (zero: halted but alive, nothing to reap)\n");

    put("[parent] SIGCONT goes out; the ticks should come back\n");
    user::Kill(kChild, cinux::syscall::kSigCont);
    spin(600000000ULL);

    put("[parent] SIGKILL on the whole group\n");
    user::Kill(static_cast<unsigned long long>(-static_cast<long long>(kChild)),
               cinux::syscall::kSigKill);
    status                  = 0;
    const long long kReaped = user::Wait4(-1, &status);
    put("[parent] wait reaped pid ");
    put_number(static_cast<unsigned long long>(kReaped));
    put(", killed by signal ");
    put_number(static_cast<unsigned long long>(status) & 0xFFULL);
    put("\n");
    user::Exit(0);
}
