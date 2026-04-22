#pragma once

#include <cstddef>
#include <cstdint>

namespace cinux::user {

int printf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

}  // namespace cinux::user
