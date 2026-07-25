/**
 * @file kernel/test/test_klog.cpp
 * @brief QEMU in-kernel tests for KernelLog (M2-2)
 *
 * Covers log/read FIFO, level filtering, dropped-on-full, and the
 * kprintf line-accumulation sink.  KernelLog is a singleton, so each
 * test clears it first.
 */

#include <stdint.h>

#include "big_kernel_test.h"
#include "kernel/lib/klog.hpp"
#include "kernel/lib/kprintf.hpp"

using cinux::lib::KernelLog;
using cinux::lib::LogLevel;
using cinux::lib::LogEntry;
using cinux::lib::set_klog_level;

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
// Test 1: log + read FIFO
// ============================================================

namespace test_klog_fifo {

void test_log_read_fifo() {
    KernelLog::instance().clear();
    klog_info("first");
    klog_info("second");

    LogEntry    out[4];
    std::size_t n = KernelLog::instance().read(out, 4);
    TEST_ASSERT_TRUE(n == 2);
    TEST_ASSERT_TRUE(contains(out[0].message, "first"));
    TEST_ASSERT_TRUE(contains(out[1].message, "second"));
    // buffer drained
    TEST_ASSERT_TRUE(KernelLog::instance().size() == 0);
}

}  // namespace test_klog_fifo

// ============================================================
// Test 2: level filtering
// ============================================================

namespace test_klog_level {

void test_level_filter() {
    KernelLog::instance().clear();
    LogLevel old = cinux::lib::g_klog_threshold;

    set_klog_level(LogLevel::WARN);
    klog_debug("dbg");  // filtered
    klog_info("inf");   // filtered
    klog_warn("wrn");   // kept
    klog_error("err");  // kept

    set_klog_level(old);
    LogEntry    out[4];
    std::size_t n = KernelLog::instance().read(out, 4);
    TEST_ASSERT_TRUE(n == 2);
    TEST_ASSERT_TRUE(contains(out[0].message, "wrn"));
    TEST_ASSERT_TRUE(contains(out[1].message, "err"));
}

}  // namespace test_klog_level

// ============================================================
// Test 3: dropped counter when ring fills
// ============================================================

namespace test_klog_dropped {

void test_dropped_when_full() {
    KernelLog::instance().clear();

    // kKlogRingSize entries fit; logging more must drop the overflow.
    for (int i = 0; i < 200; i++) {
        klog_info("x");
    }
    TEST_ASSERT_TRUE(KernelLog::instance().dropped() > 0);

    KernelLog::instance().clear();
    TEST_ASSERT_TRUE(KernelLog::instance().dropped() == 0);
}

}  // namespace test_klog_dropped

// ============================================================
// Test 4: kprintf sink accumulates a line into the history
// ============================================================

namespace test_klog_sink {

void test_kprintf_sink_accumulates() {
    KernelLog::instance().clear();
    cinux::lib::klog_init();  // register kprintf -> KernelLog sink

    cinux::lib::kprintf("[KLOGTEST] hello-line\n");

    LogEntry    out[8];
    std::size_t n = KernelLog::instance().read(out, 8);
    TEST_ASSERT_TRUE(n >= 1);

    bool found = false;
    for (std::size_t i = 0; i < n; i++) {
        if (contains(out[i].message, "hello-line")) {
            found = true;
        }
    }
    TEST_ASSERT_TRUE(found);

    KernelLog::instance().clear();
}

}  // namespace test_klog_sink

// ============================================================
// Entry point
// ============================================================

extern "C" void run_klog_tests() {
    TEST_SECTION("KernelLog Tests (M2-2)");

    RUN_TEST(test_klog_fifo::test_log_read_fifo);
    RUN_TEST(test_klog_level::test_level_filter);
    RUN_TEST(test_klog_dropped::test_dropped_when_full);
    RUN_TEST(test_klog_sink::test_kprintf_sink_accumulates);

    TEST_SUMMARY();
}
