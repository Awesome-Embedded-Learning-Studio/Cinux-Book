#include <api/syscall.hpp>

#include "cinux/ptr.hpp"

namespace {

/// A hole below the user limit with no mapping and no promise; handing
/// it to the kernel must earn an EFAULT answer, not a dead kernel.
constexpr unsigned long long kHoleAddress = 0x7000'0000'0000ULL;

void put(const char* text) {
    unsigned long long length = 0;
    while (text[length] != '\0') {
        length++;
    }
    user::Write(1, text, length);
}

void put_number(long long value) {
    char               digits[24] = {};
    unsigned long      count      = 0;
    unsigned long long magnitude  = value < 0 ? static_cast<unsigned long long>(-value)
                                              : static_cast<unsigned long long>(value);
    if (magnitude == 0) {
        put("0");
        return;
    }
    while (magnitude != 0) {
        digits[count] = static_cast<char>('0' + (magnitude % 10ULL));
        count++;
        magnitude /= 10ULL;
    }
    char          out[25] = {};
    unsigned long cursor  = 0;
    if (value < 0) {
        out[cursor] = '-';
        cursor++;
    }
    for (unsigned long index = 0; index < count; ++index) {
        out[cursor + index] = digits[count - 1 - index];
    }
    user::Write(1, out, cursor + count);
}

}  // namespace

// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" void UserMain() {
    put("[efaulter] writing four bytes at the hole address\n");
    const long long kAnswer = user::Write(1, cinux::base::PtrAt<const void>(kHoleAddress), 4);
    put("[efaulter] write answered ");
    put_number(kAnswer);
    put(" (14 is EFAULT, the door apologized)\n");
    user::Exit(kAnswer == -14 ? 0 : 1);
}
