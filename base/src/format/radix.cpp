/**
 * @file    radix.cpp
 * @brief   Number and pointer conversion helpers of the format engine.
 *
 * @author  Charliechen114514
 * @date    2026-09-25
 * @version 0.1
 * @since   0.1.0
 * @ingroup base_format
 */

#include <cstdint>

#include "detail.hpp"

namespace cinux::base::format::detail {

TextView FormatRadix(uint64_t value, unsigned base, bool uppercase, CharBuffer out) {
    if (out.capacity < 1) {
        return {};
    }

    const char* const kDigits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";

    char reversed[24];
    int  reversed_length = 0;

    do {
        reversed[reversed_length] = kDigits[value % base];
        ++reversed_length;
        value /= base;
    } while (value > 0 && reversed_length < 24);

    int written = 0;
    while (reversed_length > 0 && written < out.capacity - 1) {
        --reversed_length;
        out.data[written] = reversed[reversed_length];
        ++written;
    }
    out.data[written] = '\0';
    return {.data = out.data, .length = written};
}

TextView FormatDecimal(int64_t value, CharBuffer out) {
    if (out.capacity < 1) {
        return {};
    }

    if (value == INT64_MIN) {
        const char* const kMinText = "-9223372036854775808";
        int               written  = 0;
        while (kMinText[written] != '\0' && written < out.capacity - 1) {
            out.data[written] = kMinText[written];
            ++written;
        }
        out.data[written] = '\0';
        return {.data = out.data, .length = written};
    }

    const bool     kNegative = value < 0;
    const uint64_t kMagnitude =
        kNegative ? static_cast<uint64_t>(-value) : static_cast<uint64_t>(value);

    const int kDigitsWritten = FormatRadix(kMagnitude, 10, false, out).length;
    if (!kNegative || kDigitsWritten + 1 >= out.capacity) {
        return {.data = out.data, .length = kDigitsWritten};
    }

    for (int i = kDigitsWritten; i > 0; --i) {
        out.data[i] = out.data[i - 1];
    }
    out.data[0]                  = '-';
    out.data[kDigitsWritten + 1] = '\0';
    return {.data = out.data, .length = kDigitsWritten + 1};
}

unsigned RadixFor(char spec) {
    switch (spec) {
    case 'o':
        return 8U;
    case 'x':
    case 'X':
        return 16U;
    default:
        return 10U;
    }
}

TextView FormatPointerPrefixed(uintptr_t value, CharBuffer out) {
    const int kBodyLength = FormatRadix(value, 16, false, out).length;
    if (kBodyLength + 2 >= out.capacity) {
        return {.data = out.data, .length = kBodyLength};
    }

    for (int i = kBodyLength; i > 0; --i) {
        out.data[i + 1] = out.data[i - 1];
    }
    out.data[0] = '0';
    out.data[1] = 'x';

    const int kTotalLength = kBodyLength + 2;
    out.data[kTotalLength] = '\0';
    return {.data = out.data, .length = kTotalLength};
}

}  // namespace cinux::base::format::detail
