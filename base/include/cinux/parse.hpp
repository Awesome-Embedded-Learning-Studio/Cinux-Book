/**
 * @file    parse.hpp
 * @brief   ASCII-to-number parsing, the reading half of format.
 *
 * format.hpp spells numbers out as text; this file reads them back.
 * Fields arrive as string_view over someone else's bytes — a disk
 * format's fixed-width header, say — and leave as numbers or a loud
 * refusal. Nothing here allocates or touches OS state.
 *
 * @author  Charliechen114514
 * @date    2026-10-07
 * @version 0.1
 * @since   0.1.0
 * @ingroup base_parse
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <string_view>

#include "cinux/result.hpp"

namespace cinux::base {

/**
 * @brief         Parses an octal ASCII field into a number.
 *
 * @param[in]     field   The field bytes; leading zeros are fine, a
 *                        space or NUL ends the number.
 * @return        The value, or kInvalidArgument when a non-octal
 *                character sits inside the number.
 * @note          The ustar header's size field is the first caller:
 *                eleven octal digits plus a NUL, and an all-NUL field
 *                reads as zero.
 * @since         0.1.0
 * @ingroup       base_parse
 */
[[nodiscard]] Result<unsigned long> ParseOctal(std::string_view field);

}  // namespace cinux::base
