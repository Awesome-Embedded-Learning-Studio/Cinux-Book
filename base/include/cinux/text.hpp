/**
 * @file text.hpp
 * @brief Bounded text transfer into caller-owned storage.
 * @author Charliechen114514
 * @date 2026-10-20
 * @version 0.1
 * @since 0.3.0
 * @ingroup base_memory
 * @copyright Copyright (c) 2026
 */
#pragma once

#include <span>
#include <string_view>

#include "cinux/math.hpp"
#include "cinux/memory.hpp"

namespace cinux::base {

/**
 * @brief Copy a truncated text prefix and terminate any nonempty destination.
 * @param[out] destination Writable storage, which may be empty.
 * @param[in] source Text outside the destination storage.
 * @return Number of copied characters excluding the terminator.
 * @since 0.3.0
 * @ingroup base_memory
 */
inline unsigned long CopyText(std::span<char> destination, std::string_view source) {
    if (destination.empty()) {
        return 0;
    }
    const auto kCount = math::Min(destination.size() - 1, source.size());
    CopyBytes(destination.data(), source.data(), kCount);
    destination[kCount] = '\0';
    return kCount;
}

}
