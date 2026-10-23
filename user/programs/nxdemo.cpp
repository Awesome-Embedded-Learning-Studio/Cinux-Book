#include <api/syscall.hpp>

namespace {

/// A ret instruction sitting in writable data, right where code does not
/// belong. With the no-execute bit on its leaf, calling this earns a
/// SIGSEGV; without it the call returns and the program says so.
unsigned char g_payload[16] = {0xC3};

void put(const char* text) {
    unsigned long long length = 0;
    while (text[length] != '\0') {
        length++;
    }
    user::Write(1, text, length);
}

}  // namespace

// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" void _start() {
    put("[nxdemo] about to call into writable data\n");
    reinterpret_cast<void (*)()>(g_payload)();
    put("[nxdemo] the call returned, data pages still execute\n");
    user::Exit(0);
}
