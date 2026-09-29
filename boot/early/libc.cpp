#include <stddef.h>
#include <stdint.h>

// NOLINTBEGIN(readability-identifier-naming,misc-include-cleaner)
// The C ABI name and its locals shadow library symbols; they are ours.
extern "C" size_t strlen(char const* text) {
    size_t total = 0;
    while (text[total] != '\0') {
        ++total;
    }
    return total;
}
// NOLINTEND(readability-identifier-naming,misc-include-cleaner)

// NOLINTNEXTLINE(readability-identifier-naming,bugprone-reserved-identifier)
extern "C" uint64_t __udivmoddi4(uint64_t numerator, uint64_t denominator, uint64_t* remainder) {
    uint64_t quotient = 0;
    uint64_t rest     = 0;

    for (int bit = 63; bit >= 0; --bit) {
        rest = (rest << 1) | ((numerator >> bit) & 1U);
        if (rest >= denominator) {
            rest -= denominator;
            quotient |= (static_cast<uint64_t>(1) << bit);
        }
    }

    if (remainder != nullptr) {
        *remainder = rest;
    }
    return quotient;
}
