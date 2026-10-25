#include <api/syscall.hpp>

#include "cinux/ptr.hpp"

namespace {

/// The kernel's own text base; ring 3 may read nothing there, and with
/// SMEP open a call through it faults before the first instruction
/// retires — the supervisor-execution fence in action.
constexpr unsigned long long kKernelText = 0xFFFF'FFFF'8020'0000ULL;

void put(const char* text) {
    unsigned long long length = 0;
    while (text[length] != '\0') {
        length++;
    }
    user::Write(1, text, length);
}

}  // namespace

// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" void UserMain() {
    put("[smepdemo] about to call into kernel text\n");
    cinux::base::PtrAt<void()>(kKernelText)();
    put("[smepdemo] the call returned, kernel text ran in ring 3\n");
    user::Exit(0);
}
