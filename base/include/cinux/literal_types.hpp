/**
 * @file    literal_types.hpp
 * @brief   Unit literals: byte sizes and frequencies.
 *
 * The only place in the tree where 1024-based arithmetic may appear by
 * hand; everywhere else a size is written as 4_KiB or 16_GiB and this
 * header does the multiplying. Frequencies get the same treatment with
 * one difference: a Hertz is a strong type, because it travels through
 * interfaces where a unit-less number would leave the reader guessing.
 *
 * @author  Charliechen114514
 * @date    2026-10-02
 * @version 0.2
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

/**
 * @brief         A frequency in hertz, carried as its own type.
 * @note          Interfaces take a Hertz, never a unit-less unsigned —
 *                the type is the documentation, and no caller has to
 *                guess whether a number means hertz or kilohertz.
 * @since         0.2.0
 * @ingroup       base_literals
 */
struct Hertz {
    unsigned long long value;  ///< Oscillations per second.
};

/** @brief         Literal in hertz. */
constexpr Hertz operator""_Hz(unsigned long long value) {
    return Hertz{.value = value};
}

}  // namespace cinux::base
