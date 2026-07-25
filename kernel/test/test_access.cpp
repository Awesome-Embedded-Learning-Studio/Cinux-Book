/**
 * @file kernel/test/test_access.cpp
 * @brief In-kernel mechanism tests for sys_access (F6 / GCC self-host batch 3a)
 *
 * Drives do_access_kernel directly (the sys_ wrapper only adds a user-pointer
 * read).  Mounts a tmpfs at a unique /tmp_a* path, creates a 0644 file, and
 * checks the permission facets.  The test kernel runs as root (uid 0), so R/W
 * pass via the root bypass -- but X_OK on a 0644 file is the one denial a root
 * caller still hits (root needs an execute bit), which is the real assertion
 * here.  Cleanup-umounts so the shared mount table stays clean.
 */

#include <stdint.h>

#include "kernel/errno.hpp"
#include "kernel/fs/vfs_mount.hpp"  // vfs_resolve
#include "kernel/syscall/sys_access.hpp"
#include "kernel/syscall/sys_mount.hpp"
#include "kernel/syscall/sys_umount2.hpp"
#include "kernel/test/big_kernel_test.h"

using cinux::syscall::do_access_kernel;
using cinux::syscall::do_mount_kernel;
using cinux::syscall::do_umount2_kernel;

// Linux access(2) mode bits.
namespace {
constexpr uint32_t kFOk = 0;
constexpr uint32_t kROk = 4;
constexpr uint32_t kWOk = 2;
constexpr uint32_t kXOk = 1;

constexpr const char* kMount = "/tmp_a_mount";
constexpr const char* kFile  = "/tmp_a_mount/file";
}  // namespace

namespace test_access {

void test_access_root_bypass_and_exec_denial() {
    TEST_ASSERT_EQ(do_mount_kernel(nullptr, kMount, "tmpfs", 0), 0);
    {
        const char* rel  = nullptr;
        auto*       fs   = cinux::fs::vfs_resolve(kMount, &rel);
        auto*       root = lookup_or_null(fs, "");
        TEST_ASSERT_NOT_NULL(create_or_null(root, "file", 4));  // 0644 (tmpfs default)
    }

    // F_OK: the file exists.
    TEST_ASSERT_EQ(do_access_kernel(kFile, kFOk), 0);
    // Root bypasses R/W on a regular file.
    TEST_ASSERT_EQ(do_access_kernel(kFile, kROk), 0);
    TEST_ASSERT_EQ(do_access_kernel(kFile, kWOk), 0);
    TEST_ASSERT_EQ(do_access_kernel(kFile, kROk | kWOk), 0);
    // X_OK is denied even to root when no execute bit is set (0644).
    TEST_ASSERT_EQ(do_access_kernel(kFile, kXOk), -cinux::kEacces);

    TEST_ASSERT_EQ(do_umount2_kernel(kMount, 0), 0);
}

void test_access_negative_paths() {
    TEST_ASSERT_EQ(do_mount_kernel(nullptr, kMount, "tmpfs", 0), 0);
    {
        const char* rel = nullptr;
        auto*       fs  = cinux::fs::vfs_resolve(kMount, &rel);
        create_or_null(lookup_or_null(fs, ""), "file", 4);
    }

    // Missing path -> ENOENT.
    TEST_ASSERT_EQ(do_access_kernel("/tmp_a_mount/nope", kFOk), -cinux::kEnoent);
    // Bogus mode bits (> 7) -> EINVAL.
    TEST_ASSERT_EQ(do_access_kernel(kFile, 8), -cinux::kEinval);
    TEST_ASSERT_EQ(do_access_kernel(kFile, kROk | 0x10), -cinux::kEinval);

    TEST_ASSERT_EQ(do_umount2_kernel(kMount, 0), 0);
}

}  // namespace test_access

extern "C" void run_access_tests() {
    TEST_SECTION("access (F6-M1)");
    RUN_TEST(test_access::test_access_root_bypass_and_exec_denial);
    RUN_TEST(test_access::test_access_negative_paths);
    TEST_SUMMARY();
}
