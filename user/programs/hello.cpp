#include "api/syscall.hpp"

// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" void _start() {
    const char kFirst[]  = "[user] hello from ring 3\n";
    const char kSecond[] = "[user] two writes, now exiting\n";
    user::Write(1, kFirst, sizeof(kFirst) - 1);
    user::Write(1, kSecond, sizeof(kSecond) - 1);
    user::Exit(42);
}
