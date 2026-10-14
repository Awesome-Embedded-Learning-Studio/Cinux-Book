#include <api/syscall.hpp>

// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" void _start() {
    const char* greeting = "hello from the volume\n";
    user::Write(1, greeting, 21);
    user::Exit(42);
}
