/**
 * @file kernel/test/test_devfs.cpp
 * @brief In-kernel tests for DevFS (F6-M3)
 *
 * Runs inside QEMU via run-kernel-test.  Constructs a DevFs in-kernel with a
 * capturing sink and exercises the device nodes + readdir + stat through the
 * real InodeOps dispatch.  Mirrors the host unit tests (test/unit/test_devfs.cpp);
 * this file proves the in-kernel compile + link path and exercises the same
 * behaviour under the freestanding build.
 */

#include <stdint.h>

#include "kernel/fs/devfs/devfs.hpp"
#include "kernel/test/big_kernel_test.h"

using namespace cinux::fs;
using cinux::lib::kprintf;

namespace {

/// Kernel-side capturing sink (mirrors the host MockSink).
class CaptureSink : public CharSink {
public:
    cinux::lib::ErrorOr<int64_t> write(const void* buf, uint64_t count) override {
        const auto* b = static_cast<const uint8_t*>(buf);
        uint64_t    n = 0;
        for (; n < count && len_ < sizeof(buf_); ++n, ++len_) {
            buf_[len_] = b[n];
        }
        return static_cast<int64_t>(n);
    }
    uint8_t  buf_[64]{};
    uint64_t len_{0};
};

}  // namespace

namespace test_devfs {

// ---------- /dev/null ----------

void test_null_read_eof() {
    DevFs d(nullptr);
    TEST_ASSERT_TRUE(d.mount().ok());
    Inode*  n = d.lookup("null").value();
    char    buf[8];
    int64_t r = read_or_neg1(n, 0, buf, sizeof(buf));
    TEST_ASSERT_EQ(r, 0);
}

void test_null_write_discards() {
    DevFs d(nullptr);
    TEST_ASSERT_TRUE(d.mount().ok());
    Inode*  n = d.lookup("null").value();
    int64_t w = write_or_neg1(n, 0, "data", 4);
    TEST_ASSERT_EQ(w, 4);
}

// ---------- /dev/zero ----------

void test_zero_read_returns_zeros() {
    DevFs d(nullptr);
    TEST_ASSERT_TRUE(d.mount().ok());
    Inode* z = d.lookup("zero").value();
    char   buf[8];
    for (int i = 0; i < 8; ++i) {
        buf[i] = static_cast<char>(0xFF);
    }
    int64_t r = read_or_neg1(z, 0, buf, 8);
    TEST_ASSERT_EQ(r, 8);
    TEST_ASSERT_EQ(static_cast<int>(buf[0]), 0);
    TEST_ASSERT_EQ(static_cast<int>(buf[7]), 0);
}

// ---------- /dev/console ----------

void test_console_write_routes_to_sink() {
    CaptureSink sink;
    DevFs       d(&sink);
    TEST_ASSERT_TRUE(d.mount().ok());
    Inode*  con = d.lookup("console").value();
    int64_t w   = write_or_neg1(con, 0, "AB", 2);
    TEST_ASSERT_EQ(w, 2);
    TEST_ASSERT_EQ(sink.len_, static_cast<uint64_t>(2));
    TEST_ASSERT_EQ(static_cast<int>(sink.buf_[0]), 'A');
    TEST_ASSERT_EQ(static_cast<int>(sink.buf_[1]), 'B');
}

// ---------- lookup ----------

void test_lookup_nodes_and_root() {
    DevFs d(nullptr);
    TEST_ASSERT_TRUE(d.mount().ok());
    TEST_ASSERT_NOT_NULL(lookup_or_null(&d, "null"));
    TEST_ASSERT_NOT_NULL(lookup_or_null(&d, "zero"));
    TEST_ASSERT_NOT_NULL(lookup_or_null(&d, "console"));
    TEST_ASSERT_NULL(lookup_or_null(&d, "missing"));
    Inode* root = lookup_or_null(&d, "");
    TEST_ASSERT_NOT_NULL(root);
    TEST_ASSERT_EQ(static_cast<int>(root->type), static_cast<int>(InodeType::Directory));
}

// ---------- readdir ----------

void test_readdir_order() {
    DevFs d(nullptr);
    TEST_ASSERT_TRUE(d.mount().ok());
    Inode* root = lookup_or_null(&d, "");
    char   name[DEVFS_NAME_MAX];

    TEST_ASSERT_EQ(readdir_or_neg1(root, 0, name, sizeof(name)), 1);
    TEST_ASSERT_EQ(name[0], '.');
    TEST_ASSERT_EQ(name[1], '\0');

    TEST_ASSERT_EQ(readdir_or_neg1(root, 1, name, sizeof(name)), 1);

    TEST_ASSERT_EQ(readdir_or_neg1(root, 2, name, sizeof(name)), 1);
    TEST_ASSERT_TRUE(name[0] == 'n');  // "null"

    TEST_ASSERT_EQ(readdir_or_neg1(root, 5, name, sizeof(name)), 0);  // exhausted
}

// ---------- stat ----------

void test_stat_device_numbers() {
    DevFs       d(nullptr);
    struct stat st;
    TEST_ASSERT_TRUE(d.mount().ok());

    Inode* n = lookup_or_null(&d, "null");
    TEST_ASSERT_TRUE(n->ops->stat(n, &st).ok());
    TEST_ASSERT_EQ(st.st_mode, static_cast<uint32_t>(kSIfChr | 0666));
    TEST_ASSERT_EQ(st.st_rdev, devfs_makedev(1, 3));

    Inode* c = lookup_or_null(&d, "console");
    TEST_ASSERT_TRUE(c->ops->stat(c, &st).ok());
    TEST_ASSERT_EQ(st.st_rdev, devfs_makedev(5, 1));
}

}  // namespace test_devfs

// ============================================================
// Entry point
// ============================================================

extern "C" void run_devfs_tests() {
    TEST_SECTION("DevFS (F6-M3)");
    RUN_TEST(test_devfs::test_null_read_eof);
    RUN_TEST(test_devfs::test_null_write_discards);
    RUN_TEST(test_devfs::test_zero_read_returns_zeros);
    RUN_TEST(test_devfs::test_console_write_routes_to_sink);
    RUN_TEST(test_devfs::test_lookup_nodes_and_root);
    RUN_TEST(test_devfs::test_readdir_order);
    RUN_TEST(test_devfs::test_stat_device_numbers);
    TEST_SUMMARY();
}
