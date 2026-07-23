/**
 * @file kernel/test/test_sys_dmesg.cpp
 * @brief QEMU in-kernel tests for sys_dmesg (M2-3)
 *
 * Exercises the syscall handler directly (kernel-side): syscall number,
 * invalid-buffer fault, and entry formatting.
 */

#include <stdint.h>

#include "big_kernel_test.h"
#include "kernel/lib/klog.hpp"
#include "kernel/syscall/sys_dmesg.hpp"
#include "kernel/syscall/syscall_nums.hpp"
#include "kernel/test/user_page.hpp"

using cinux::lib::KernelLog;
using cinux::syscall::SyscallNr;
using cinux::syscall::sys_dmesg;

namespace {

// Minimal substring search (no libc dependency).
bool contains(const char* s, const char* sub) {
    if (sub[0] == '\0') {
        return true;
    }
    for (; s[0] != '\0'; ++s) {
        const char* a = s;
        const char* b = sub;
        while (a[0] != '\0' && b[0] != '\0' && a[0] == b[0]) {
            ++a;
            ++b;
        }
        if (b[0] == '\0') {
            return true;
        }
    }
    return false;
}

}  // namespace

// ============================================================
// Test 1: syscall number
// ============================================================

namespace test_sys_dmesg {

constexpr uint64_t kDmesgUserPage = 0x0000000061300000ULL;

void test_sys_dmesg_number() {
    TEST_ASSERT_TRUE(static_cast<uint64_t>(SyscallNr::SYS_dmesg) == 103);
}

// ============================================================
// Test 2: null / invalid buffer -> -errno
// ============================================================

void test_sys_dmesg_null_buf_returns_fault() {
    KernelLog::instance().clear();
    klog_info("x");

    int64_t r = sys_dmesg(0, 100, 0, 0, 0, 0);
    TEST_ASSERT_TRUE(r < 0);  // -EFAULT

    KernelLog::instance().clear();
}

// ============================================================
// Test 3: formats entries as "[LEVEL] tick: message"
// ============================================================

void test_sys_dmesg_formats_entries() {
    KernelLog::instance().clear();
    klog_error("boom");
    klog_info("hello");

    cinux::test::UserPage page(kDmesgUserPage);
    TEST_ASSERT_TRUE(page.ok());
    int64_t r = sys_dmesg(page.addr(), 1024, 0, 0, 0, 0);
    TEST_ASSERT_TRUE(r > 0);
    char buf[1024] = {};  // zero-init so contains() stops at the written end
    TEST_ASSERT_TRUE(page.read_bytes(0, buf, static_cast<size_t>(r)));
    TEST_ASSERT_TRUE(contains(buf, "ERROR"));
    TEST_ASSERT_TRUE(contains(buf, "boom"));
    TEST_ASSERT_TRUE(contains(buf, "INFO"));
    TEST_ASSERT_TRUE(contains(buf, "hello"));

    KernelLog::instance().clear();
}

}  // namespace test_sys_dmesg

// ============================================================
// Entry point
// ============================================================

extern "C" void run_sys_dmesg_tests() {
    TEST_SECTION("sys_dmesg Tests (M2-3)");

    RUN_TEST(test_sys_dmesg::test_sys_dmesg_number);
    RUN_TEST(test_sys_dmesg::test_sys_dmesg_null_buf_returns_fault);
    RUN_TEST(test_sys_dmesg::test_sys_dmesg_formats_entries);

    TEST_SUMMARY();
}
