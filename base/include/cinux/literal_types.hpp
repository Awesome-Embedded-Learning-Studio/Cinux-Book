/**
 * @file    literal_types.hpp
 * @brief   Byte-unit user-defined literals for memory sizes.
 *
 * The only place in the tree where 1024-based arithmetic may appear by
 * hand; everywhere else a size is written as 4_KiB or 16_GiB and this
 * header does the multiplying.
 *
 * @author  Charliechen114514
 * @date    2026-10-02
 * @version 0.1
 * @since   0.1.0
 * @ingroup base_literals
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <stdint.h>

namespace cinux::base {

/** @brief         Literal in kibibytes (1024-based). */
constexpr unsigned long long operator""_KiB(unsigned long long value) {
    return value * 1024ULL;
}

/** @brief         Literal in mebibytes (1024-based). */
constexpr unsigned long long operator""_MiB(unsigned long long value) {
    return value * 1024ULL * 1024ULL;
}

/** @brief         Literal in gibibytes (1024-based). */
constexpr unsigned long long operator""_GiB(unsigned long long value) {
    return value * 1024ULL * 1024ULL * 1024ULL;
}

/** @brief         Literal in tebibytes (1024-based). */
constexpr unsigned long long operator""_TiB(unsigned long long value) {
    return value * 1024ULL * 1024ULL * 1024ULL * 1024ULL;
}

}  // namespace cinux::base
