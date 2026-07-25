/**
 * @file kernel/mini/mm/memory_literals.h
 * @brief Custom Literal Operators for Memory Sizes
 *
 * Delegates to Cinux-Base's cinux::lib::literals.
 */

#pragma once

#include <cinux/numeric.hpp>

namespace cinux::mini::mm {

// Import Cinux-Base memory literals into mini-kernel namespace
using namespace cinux::lib::literals;

}  // namespace cinux::mini::mm
