/**
 * @file    assert_host.cpp
 * @brief   Host-world definition of the assertion failure hook.
 *
 * @author  Charliechen114514
 * @date    2026-09-25
 * @version 0.1
 * @since   0.1.0
 * @ingroup base_safety
 */

#include <cinux/assert.hpp>
#include <cstdlib>
#include <print>
#include <source_location>

namespace cinux::base::safety {

void AssertionFailed(const char* k_message, std::source_location loc) {
    std::println(stderr, "[assert] {}:{}:{} in {}: {}", loc.file_name(), loc.line(), loc.column(),
                 loc.function_name(), k_message);
    std::abort();
}

}  // namespace cinux::base::safety
