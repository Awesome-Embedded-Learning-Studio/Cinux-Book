/**
 * @file    parse.cpp
 * @brief   The reading half of format: text fields back into numbers.
 *
 * @author  Charliechen114514
 * @date    2026-10-07
 * @version 0.1
 * @since   0.1.0
 * @ingroup base_parse
 * @copyright Copyright (c) 2026
 */

#include "cinux/parse.hpp"

#include <string_view>

#include "cinux/result.hpp"

namespace cinux::base {

Result<unsigned long> ParseOctal(std::string_view field) {
    unsigned long value = 0;
    for (const char kDigit : field) {
        if (kDigit == ' ' || kDigit == 0) {
            break;
        }
        if (kDigit < '0' || kDigit > '7') {
            return KernelError::kInvalidArgument;
        }
        value = (value * 8) + static_cast<unsigned long>(kDigit - '0');
    }
    return value;
}

}  // namespace cinux::base
