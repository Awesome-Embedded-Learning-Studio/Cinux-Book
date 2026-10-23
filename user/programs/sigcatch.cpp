#include <api/syscall.hpp>

#include "kernel/syscall/signal.hpp"

namespace {

volatile unsigned long long g_caught = 0;

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

/// The caught counter lives in the handler and nowhere else; a wrong
/// return path never brings execution back here, so the number below
/// proves the whole trampoline round trip.
void on_usr1(int /*number*/) {
    g_caught = g_caught + 1ULL;
}

}  // namespace

// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" void _start() {
    cinux::syscall::SigAction action{};
    action.handler       = reinterpret_cast<unsigned long long>(&on_usr1);
    const long long kSet = user::Sigaction(cinux::syscall::kSigUsr1, &action, nullptr);
    if (kSet != 0) {
        put("[sigcatch] arming the handler refused\n");
        user::Exit(1);
    }

    put("[sigcatch] handler armed without a restorer, kernel trampoline owns the return\n");
    const long long kRaised =
        user::Kill(static_cast<unsigned long long>(user::Getpid()), cinux::syscall::kSigUsr1);
    if (kRaised != 0) {
        put("[sigcatch] raise refused\n");
        user::Exit(1);
    }

    unsigned long long spins = 0;
    while (g_caught == 0 && spins < 800000000ULL) {
        spins++;
    }
    put("[sigcatch] caught ");
    put_number(g_caught);
    put(" after ");
    put_number(spins);
    put(" spins\n");
    user::Exit(g_caught == 1 ? 0 : 2);
}
