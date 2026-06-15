/**
 * @file kernel/test/big_kernel_test.h
 * @brief Lightweight test framework for the big kernel
 *
 * Adapted from kernel/mini/test/kernel_test.h but uses the big kernel's
 * kprintf (cinux::lib::kprintf) instead of the mini kernel's version.
 *
 * Usage:
 *   1. Include this header in your test file
 *   2. Use TEST_ASSERT_* macros for assertions
 *   3. Use RUN_TEST(fn) to execute test functions
 *   4. Call TEST_SUMMARY() to print results
 *
 * Exit mechanism: QEMU isa-debug-exit device (port 0xf4)
 *   - Write 0 = success (QEMU exits with code 1)
 *   - Write 1 = failure (QEMU exits with code 3)
 */

#pragma once

#include "kernel/lib/kprintf.hpp"
#include "kernel/fs/vfs_filesystem.hpp"

using cinux::lib::kprintf;

// ============================================================
// Test State Management
// ============================================================

namespace test {
inline int tests_passed = 0;
inline int tests_failed = 0;

inline void reset() {
    tests_passed = 0;
    tests_failed = 0;
}

inline int total() {
    return tests_passed + tests_failed;
}

inline bool all_passed() {
    return tests_failed == 0;
}

inline int get_total_failed() {
    return tests_failed;
}
}  // namespace test

// ============================================================
// Filesystem test helper
// ============================================================

/**
 * @brief Run a VFS lookup and collapse the result to a raw pointer.
 *
 * FileSystem::lookup() now returns ErrorOr<Inode*>.  Many integration tests
 * only care about found vs. not-found and keep their TEST_ASSERT_NOT_NULL /
 * null-fallback style unchanged — this helper adapts the ErrorOr result back
 * to a nullable pointer so those tests need no logic changes.
 *
 * @return The found Inode, or nullptr on any lookup error.
 */
inline cinux::fs::Inode* lookup_or_null(cinux::fs::FileSystem* fs, const char* path) {
    auto result = fs->lookup(path);
    return result.ok() ? result.value() : nullptr;
}

/**
 * @brief Run InodeOps::create and collapse ErrorOr<Inode*> to a nullable pointer.
 *
 * Lets legacy tests keep TEST_ASSERT_NULL / TEST_ASSERT_NOT_NULL style after
 * create() moved to ErrorOr<Inode*>.
 */
inline cinux::fs::Inode* create_or_null(cinux::fs::Inode* dir, const char* name, uint32_t len) {
    auto r = dir->ops->create(dir, name, len);
    return r.ok() ? r.value() : nullptr;
}

/** @brief Same as create_or_null but for InodeOps::mkdir. */
inline cinux::fs::Inode* mkdir_or_null(cinux::fs::Inode* dir, const char* name, uint32_t len) {
    auto r = dir->ops->mkdir(dir, name, len);
    return r.ok() ? r.value() : nullptr;
}

/**
 * @brief Run InodeOps::unlink and return a POSIX-style result code.
 *
 * @return 0 on success, -1 on any error — keeps TEST_ASSERT_EQ(rc, 0/-1) style.
 */
inline int64_t unlink_rc(cinux::fs::Inode* dir, const char* name, uint32_t len) {
    return dir->ops->unlink(dir, name, len).ok() ? 0 : -1;
}

/**
 * @brief Run InodeOps::read and collapse ErrorOr<int64_t> to a raw code.
 *
 * @return The byte count (0 = EOF) on success, or -1 on any error — keeps
 * TEST_ASSERT_EQ(n, <count>) / TEST_ASSERT_GE(n, 0) style unchanged.
 */
inline int64_t read_or_neg1(const cinux::fs::Inode* inode, uint64_t offset, void* buf, uint64_t count) {
    auto r = inode->ops->read(inode, offset, buf, count);
    return r.ok() ? r.value() : -1;
}

/** @brief Same as read_or_neg1 but for InodeOps::write. */
inline int64_t write_or_neg1(cinux::fs::Inode* inode, uint64_t offset, const void* buf, uint64_t count) {
    auto r = inode->ops->write(inode, offset, buf, count);
    return r.ok() ? r.value() : -1;
}

/**
 * @brief Same as read_or_neg1 but for InodeOps::readdir.
 *
 * @return 1 when an entry was filled, 0 when the directory is exhausted, or
 * -1 on any error — preserves the pre-ErrorOr call-site semantics.
 */
inline int64_t readdir_or_neg1(const cinux::fs::Inode* inode, uint64_t index, char* name, uint64_t name_max) {
    auto r = inode->ops->readdir(inode, index, name, name_max);
    return r.ok() ? r.value() : -1;
}

// ============================================================
// Assertion Macros
// ============================================================

#define TEST_ASSERT(cond)                                                                          \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            kprintf("[FAIL] %s at %s:%d\n", #cond, __FILE__, __LINE__);                            \
            test::tests_failed++;                                                                  \
            return;                                                                                \
        }                                                                                          \
    } while (0)

#define TEST_ASSERT_EQ(a, b)      TEST_ASSERT((a) == (b))
#define TEST_ASSERT_NE(a, b)      TEST_ASSERT((a) != (b))
#define TEST_ASSERT_GT(a, b)      TEST_ASSERT((a) > (b))
#define TEST_ASSERT_GE(a, b)      TEST_ASSERT((a) >= (b))
#define TEST_ASSERT_LT(a, b)      TEST_ASSERT((a) < (b))
#define TEST_ASSERT_LE(a, b)      TEST_ASSERT((a) <= (b))
#define TEST_ASSERT_NULL(ptr)     TEST_ASSERT((ptr) == nullptr)
#define TEST_ASSERT_NOT_NULL(ptr) TEST_ASSERT((ptr) != nullptr)
#define TEST_ASSERT_TRUE(expr)    TEST_ASSERT((expr) == true)
#define TEST_ASSERT_FALSE(expr)   TEST_ASSERT((expr) == false)

// ============================================================
// Test Runner Macros
// ============================================================

#define RUN_TEST(fn)                                                                               \
    do {                                                                                           \
        kprintf("[RUN] %s\n", #fn);                                                                \
        int _failed_before = test::tests_failed;                                                   \
        fn();                                                                                      \
        if (test::tests_failed == _failed_before) {                                                \
            test::tests_passed++;                                                                  \
            kprintf("[PASS] %s\n", #fn);                                                           \
        }                                                                                          \
    } while (0)

#define TEST_SUMMARY()                                                                             \
    do {                                                                                           \
        kprintf("\n=== Tests: %d passed, %d failed ===\n", test::tests_passed,                     \
                test::tests_failed);                                                               \
    } while (0)

#define TEST_SECTION(name)                                                                         \
    do {                                                                                           \
        kprintf("\n=== %s ===\n", name);                                                           \
    } while (0)
