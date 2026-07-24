/**
 * @file kernel/test/test_dentry.cpp
 * @brief In-kernel mechanism tests for DentryCache (F6-M1 B3)
 *
 * Drives DentryCache directly with stack Inode sentinels (the cache keys on the
 * Inode* address and pins the child via inode_ref -- it never dereferences
 * either). Exercises add/lookup hit + miss + invalidate. vfs_lookup integration
 * (a hit skips lookup_child) is covered by the existing path tests running
 * green with the cache wired in.
 */

#include <stdint.h>

#include "kernel/fs/dentry.hpp"
#include "kernel/fs/file.hpp"  // inode_unref (balance lookup's ref)
#include "kernel/fs/inode.hpp"
#include "kernel/test/big_kernel_test.h"

using cinux::fs::DentryCache;
using cinux::fs::Inode;

namespace test_dentry {

void test_dentry_add_then_lookup_hits() {
    Inode parent{};
    Inode child{};
    DentryCache::add(&parent, "foo", 3, &child);
    Inode* hit = DentryCache::lookup(&parent, "foo", 3);
    TEST_ASSERT_EQ(hit, &child);  // cached child returned, ref'd
    inode_unref(hit);             // balance lookup's ref
    DentryCache::invalidate(&parent, "foo", 3);
}

void test_dentry_unknown_name_misses() {
    Inode parent{};
    TEST_ASSERT_NULL(DentryCache::lookup(&parent, "bar", 3));
    TEST_ASSERT_NULL(DentryCache::lookup(&parent, "foo", 3));
}

void test_dentry_invalidate_then_miss() {
    Inode parent{};
    Inode child{};
    DentryCache::add(&parent, "name", 4, &child);
    DentryCache::invalidate(&parent, "name", 4);
    TEST_ASSERT_NULL(DentryCache::lookup(&parent, "name", 4));
}

void test_dentry_invalidate_absent_is_noop() {
    Inode parent{};
    DentryCache::invalidate(&parent, "never", 5);  // must not crash
    TEST_ASSERT_NULL(DentryCache::lookup(&parent, "never", 5));
}

void test_dentry_distinct_parents_distinct_entries() {
    Inode p1{}, p2{}, c1{};
    DentryCache::add(&p1, "x", 1, &c1);
    TEST_ASSERT_EQ(DentryCache::lookup(&p1, "x", 1), &c1);
    TEST_ASSERT_NULL(DentryCache::lookup(&p2, "x", 1));  // different parent -> miss
    Inode* hit = DentryCache::lookup(&p1, "x", 1);
    inode_unref(hit);
    DentryCache::invalidate(&p1, "x", 1);
}

}  // namespace test_dentry

extern "C" void run_dentry_tests() {
    TEST_SECTION("DentryCache (F6-M1 B3)");
    RUN_TEST(test_dentry::test_dentry_add_then_lookup_hits);
    RUN_TEST(test_dentry::test_dentry_unknown_name_misses);
    RUN_TEST(test_dentry::test_dentry_invalidate_then_miss);
    RUN_TEST(test_dentry::test_dentry_invalidate_absent_is_noop);
    RUN_TEST(test_dentry::test_dentry_distinct_parents_distinct_entries);
    TEST_SUMMARY();
}
