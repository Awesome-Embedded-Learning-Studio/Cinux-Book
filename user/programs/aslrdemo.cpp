#include <api/syscall.hpp>

namespace {

void put(const char* text) {
    unsigned long long length = 0;
    while (text[length] != '\0') {
        length++;
    }
    user::Write(1, text, length);
}

void put_hex(unsigned long long value) {
    const char* const kDigits = "0123456789ABCDEF";
    char              out[19] = "0x";
    for (unsigned index = 0; index < 16; ++index) {
        out[2 + index] = kDigits[(value >> (60U - (4U * index))) & 0xFULL];
    }
    user::Write(1, out, 18);
}

unsigned long long stack_pointer() {
    unsigned long long rsp = 0;
    asm volatile("mov %%rsp, %0" : "=r"(rsp));
    return rsp;
}

}  // namespace

// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" void _start() {
    const unsigned long long kBreak = user::Brk(0);
    const long long          kPage  = user::Mmap(0, 4096, 3, 0x22, -1, 0);
    put("[aslrdemo] rsp=");
    put_hex(stack_pointer());
    put(" brk=");
    put_hex(kBreak);
    put(" mmap=");
    put_hex(static_cast<unsigned long long>(kPage));
    put(" uid=");
    put_hex(static_cast<unsigned long long>(user::Getuid()));
    put("\n");
    user::Exit(0);
}
