/**
 * @file    framework_kernel.cpp
 * @brief   Linker-section runner of the kernel-world test framework.
 *
 * The table is pure data the linker already laid out: g_test_start through
 * g_test_end in kernel.ld. Walking it needs no constructor, no heap, and
 * no static-initialization order — the properties that make a linker
 * section the only registration channel a kernel can trust at this stage.
 * Each case announces itself with a [RUN] line before it executes, so a
 * wrecked global that kills the machine leaves the culprit named as the
 * last line on the serial console.
 *
 * @author  Charliechen114514
 * @date    2026-10-02
 * @version 0.2
 * @since   0.1.0
 * @ingroup test_framework
 * @copyright Copyright (c) 2026
 */

#include "framework_kernel.hpp"

#include <source_location>

#include "kernel/arch/x86_64/halt.hpp"
#include "kernel/boot/print.hpp"
#include "kernel/driver/base/io.hpp"
#include "test_assert.hpp"
#include "test_case.hpp"

extern "C" {

/// First test-case record; defined by kernel.ld as the .test_cases fence.
extern cinux::test::TestCase g_test_start[];
/// One-past-last test-case record; defined by kernel.ld as the fence.
extern cinux::test::TestCase g_test_end[];
}

namespace cinux::test {

namespace {

constexpr unsigned short kQemuDebugExitPort = 0xF4;

/// Failure-line vocabulary of ReportCompareFailure, indexed by the
/// expected_equal flag: false wants difference, true wants equality.
constexpr const char* const kCompareNames[2] = {"ASSERT_NE", "ASSERT_EQ"};

const char* g_current_case_name = "";

bool run_single(const TestCase& test_case) {
    const int         kFailuresBefore = g_failures;
    const char* const kNameBefore     = g_current_case_name;

    g_current_case_name = test_case.name;
    test_case.body();

    g_current_case_name = kNameBefore;
    const bool kPassed  = g_failures == kFailuresBefore;
    g_failures          = kFailuresBefore;
    return kPassed;
}

/**
 * @brief         Terminates QEMU through the isa-debug-exit device.
 *
 * @param[in]     code   Payload written to port 0xF4 as-is; the device
 *                       itself exits QEMU with (code << 1) + 1, so code 1
 *                       (all passed) exits 3.
 * @return        None
 * @note          A plain park follows the port write for any machine
 *                without the device — the write itself is harmless there.
 * @since         0.1.0
 * @ingroup       test_framework
 */
[[noreturn]] void exit_qemu(unsigned char code) {
    cinux::driver::OutB(cinux::driver::PortWrite{.port = kQemuDebugExitPort, .value = code});
    cinux::arch::Halt();
}

}  // namespace

int g_failures = 0;

const char* CurrentCaseName() {
    return g_current_case_name;
}

void ReportCheckFailure(const char* expression, std::source_location loc) {
    cinux::print::Println("[FAIL] %s\n  ASSERT_TRUE(%s) failed\n  at %s:%u", CurrentCaseName(),
                          expression, loc.file_name(), loc.line());
}

void ReportCompareFailure(const char* actual_text, const char* expected_text, bool expected_equal,
                          std::source_location loc) {
    cinux::print::Println("[FAIL] %s\n  %s(%s, %s) failed\n  at %s:%u", CurrentCaseName(),
                          kCompareNames[expected_equal ? 1 : 0], actual_text, expected_text,
                          loc.file_name(), loc.line());
}

void RunKernelTests() {
    // NOLINTNEXTLINE(clang-analyzer-security.PointerSub)
    const auto kCount = static_cast<unsigned long>(g_test_end - g_test_start);
    if (kCount == 0) {
        return;
    }

    cinux::print::Println("");
    cinux::print::Println("=== Cinux Kernel Test Runner ===");
    cinux::print::Println("Running %u case(s)...", kCount);
    cinux::print::Println("");

    int failed = 0;
    for (unsigned long index = 0; index < kCount; ++index) {
        const TestCase& current_case = g_test_start[index];
        cinux::print::Println("[RUN] %s", current_case.name);
        if (run_single(current_case)) {
            cinux::print::Println("[PASS] %s", current_case.name);
        } else {
            cinux::print::Println("[FAIL] %s", current_case.name);
            ++failed;
        }
    }

    cinux::print::Println("");
    cinux::print::Println(
        "=== Results: %u passed, %u failed ===", kCount - static_cast<unsigned long>(failed),
        static_cast<unsigned long>(failed));

    exit_qemu(static_cast<unsigned char>(failed + 1));
}

}  // namespace cinux::test
