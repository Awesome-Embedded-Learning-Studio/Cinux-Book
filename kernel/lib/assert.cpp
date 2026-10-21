/**
 * @file    assert.cpp
 * @brief   Kernel-world definition of the assertion failure hook.
 *
 * The twin of test/framework/assert_host.cpp: same declaration from
 * cinux/assert.hpp, selected at link time. A violated Check in the kernel
 * is not recoverable — the hook prints one panic line with the capture
 * point and parks the machine, never touching state again.
 *
 * @author  Charliechen114514
 * @date    2026-10-02
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_lib
 * @copyright Copyright (c) 2026
 */

#include <cinux/assert.hpp>
#include <source_location>

#include "kernel/boot/print.hpp"
#include "kernel/debug/panic.hpp"

namespace cinux::base::safety {

void AssertionFailed(const char* k_message, std::source_location loc) {
    cinux::print::Println("[panic] %s", k_message);
    cinux::print::Println("[kern] %s:%u in %s", loc.file_name(), loc.line(), loc.function_name());
    unsigned long long frame = 0;  // NOLINT(misc-const-correctness) written by the asm output
    __asm__ volatile("movq %%rbp, %0" : "=r"(frame));
    cinux::debug::PanicDump(frame);
}

}  // namespace cinux::base::safety
