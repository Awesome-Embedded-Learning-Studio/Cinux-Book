/**
 * @file kernel/test/test_tmpfs.cpp
 * @brief In-kernel mechanism tests for TmpFs (F6-M4 / GCC self-host batch 1)
 *
 * Runs inside QEMU via run-kernel-test.  TmpFs is pure in-memory logic, so a
 * stack-local TmpFs instance is exercised directly through its FileSystem /
 * InodeOps -- no global mount table, no real syscalls -- which keeps every
 * assertion deterministic (the ring-0 test kernel shares a global fd table and
 * mount table across sections; touching them here would bleed into later tests).
 *
 * Coverage: mount + root, create/write/read round-trip, append + offset read +
 * gap-zeroing past the 4 KiB growth boundary, mkdir + nested multi-component
 * lookup + readdir, stat (file + dir), unlink (file + empty dir), and the
 * negative paths (missing entry, duplicate create, descend through a file).
 */

#include <stdint.h>

#include "kernel/fs/tmpfs/tmpfs.hpp"
#include "kernel/lib/string.hpp"  // strlen / memcmp
#include "kernel/test/big_kernel_test.h"

using namespace cinux::fs;
using cinux::lib::kprintf;

namespace {

/// Length of @p s as a uint32_t (the InodeOps name-length parameter type).
uint32_t nlen(const char* s) {
    return static_cast<uint32_t>(strlen(s));
}

/// Fill @p buf (count bytes) with byte value @p v.
void fill_buf(uint8_t* buf, uint64_t count, uint8_t v) {
    for (uint64_t i = 0; i < count; ++i) {
        buf[i] = v;
    }
}

/// True iff @p buf is @p count bytes all equal to @p v.
bool buf_all(const uint8_t* buf, uint64_t count, uint8_t v) {
    for (uint64_t i = 0; i < count; ++i) {
        if (buf[i] != v) {
            return false;
        }
    }
    return true;
}

}  // namespace

namespace test_tmpfs {

// ---------- mount + root ----------

void test_mount_and_root() {
    TmpFs tfs;
    TEST_ASSERT_TRUE(tfs.mount().ok());

    Inode* root = lookup_or_null(&tfs, "");
    TEST_ASSERT_NOT_NULL(root);
    TEST_ASSERT_EQ(static_cast<int>(root->type), static_cast<int>(InodeType::Directory));

    // mount() is idempotent (mirrors DevFs / ProcFs).
    TEST_ASSERT_TRUE(tfs.mount().ok());

    // Root is also reachable via "/".
    TEST_ASSERT_NOT_NULL(lookup_or_null(&tfs, "/"));
}

// ---------- create / write / read ----------

void test_create_write_read_roundtrip() {
    TmpFs tfs;
    TEST_ASSERT_TRUE(tfs.mount().ok());
    Inode* root = lookup_or_null(&tfs, "");

    Inode* f = create_or_null(root, "hello.txt", nlen("hello.txt"));
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQ(static_cast<int>(f->type), static_cast<int>(InodeType::Regular));

    const char* msg     = "hello tmpfs";
    int64_t     written = write_or_neg1(f, 0, msg, nlen(msg));
    TEST_ASSERT_EQ(written, static_cast<int64_t>(nlen(msg)));

    char    buf[32];
    int64_t nread = read_or_neg1(f, 0, buf, sizeof(buf));
    TEST_ASSERT_EQ(nread, static_cast<int64_t>(nlen(msg)));
    buf[nread] = '\0';
    TEST_ASSERT_EQ(memcmp(buf, msg, static_cast<uint32_t>(nread)), 0);

    // Reading past EOF returns 0.
    TEST_ASSERT_EQ(read_or_neg1(f, nlen(msg), buf, sizeof(buf)), 0);
}

void test_append_and_offset_read() {
    TmpFs tfs;
    TEST_ASSERT_TRUE(tfs.mount().ok());
    Inode* root = lookup_or_null(&tfs, "");
    Inode* f    = create_or_null(root, "f", 1);
    TEST_ASSERT_NOT_NULL(f);

    write_or_neg1(f, 0, "abc", 3);
    write_or_neg1(f, 3, "def", 3);  // append at EOF -> "abcdef"

    char buf[8];
    TEST_ASSERT_EQ(read_or_neg1(f, 1, buf, 4), 4);  // offset read: "bcde"
    TEST_ASSERT_EQ(memcmp(buf, "bcde", 4), 0);
    TEST_ASSERT_EQ(read_or_neg1(f, 0, buf, sizeof(buf)), 6);  // whole file
    TEST_ASSERT_EQ(memcmp(buf, "abcdef", 6), 0);
}

void test_truncate_shrink_and_extend() {
    TmpFs tfs;
    TEST_ASSERT_TRUE(tfs.mount().ok());
    Inode* root = lookup_or_null(&tfs, "");
    Inode* f    = create_or_null(root, "trunc", 5);
    TEST_ASSERT_NOT_NULL(f);

    TEST_ASSERT_EQ(write_or_neg1(f, 0, "abcdef", 6), 6);
    TEST_ASSERT_TRUE(f->ops->truncate(f, 3).ok());
    TEST_ASSERT_EQ(f->size, 3u);

    char buf[8];
    TEST_ASSERT_EQ(read_or_neg1(f, 0, buf, sizeof(buf)), 3);
    TEST_ASSERT_EQ(memcmp(buf, "abc", 3), 0);

    TEST_ASSERT_TRUE(f->ops->truncate(f, 6).ok());
    TEST_ASSERT_EQ(f->size, 6u);
    TEST_ASSERT_EQ(read_or_neg1(f, 0, buf, sizeof(buf)), 6);
    TEST_ASSERT_EQ(memcmp(buf, "abc", 3), 0);
    TEST_ASSERT_TRUE(buf_all(reinterpret_cast<const uint8_t*>(buf + 3), 3, 0));
}

// Write 50 bytes, then write another 50 at offset 4090 -- the second write
// straddles the 4 KiB growth boundary, so the buffer must grow to 8192 and the
// gap [50, 4090) must be zero-filled (not stale heap bytes).
void test_grow_past_4k_boundary_and_gap() {
    TmpFs tfs;
    TEST_ASSERT_TRUE(tfs.mount().ok());
    Inode* root = lookup_or_null(&tfs, "");
    Inode* f    = create_or_null(root, "big", 3);
    TEST_ASSERT_NOT_NULL(f);

    uint8_t a[50];
    uint8_t b[50];
    fill_buf(a, sizeof(a), 'A');
    fill_buf(b, sizeof(b), 'B');

    TEST_ASSERT_EQ(write_or_neg1(f, 0, a, sizeof(a)), static_cast<int64_t>(sizeof(a)));
    TEST_ASSERT_EQ(write_or_neg1(f, 4090, b, sizeof(b)),
                   static_cast<int64_t>(sizeof(b)));  // forces growth past 4096

    uint8_t out[50];
    // The 'B' block at [4090, 4140).
    TEST_ASSERT_EQ(read_or_neg1(f, 4090, out, sizeof(out)), static_cast<int64_t>(sizeof(out)));
    TEST_ASSERT_TRUE(buf_all(out, sizeof(out), 'B'));
    // The 'A' block at [0, 50).
    TEST_ASSERT_EQ(read_or_neg1(f, 0, out, sizeof(out)), static_cast<int64_t>(sizeof(out)));
    TEST_ASSERT_TRUE(buf_all(out, sizeof(out), 'A'));
    // The gap [50, 100) is zero (write beyond old EOF zero-fills).
    TEST_ASSERT_EQ(read_or_neg1(f, 50, out, 50), 50);
    TEST_ASSERT_TRUE(buf_all(out, 50, 0));
}

// ---------- mkdir + readdir + nested lookup ----------

void test_mkdir_readdir_and_nested_lookup() {
    TmpFs tfs;
    TEST_ASSERT_TRUE(tfs.mount().ok());
    Inode* root = lookup_or_null(&tfs, "");

    Inode* sub = mkdir_or_null(root, "sub", 3);
    TEST_ASSERT_NOT_NULL(sub);
    TEST_ASSERT_EQ(static_cast<int>(sub->type), static_cast<int>(InodeType::Directory));

    char name[16];
    TEST_ASSERT_EQ(readdir_or_neg1(root, 0, name, sizeof(name)), 1);  // "."
    TEST_ASSERT_EQ(readdir_or_neg1(root, 1, name, sizeof(name)), 1);  // ".."
    TEST_ASSERT_EQ(readdir_or_neg1(root, 2, name, sizeof(name)), 1);
    TEST_ASSERT_EQ(strcmp(name, "sub"), 0);
    TEST_ASSERT_EQ(readdir_or_neg1(root, 3, name, sizeof(name)), 0);  // end

    // Create a file inside sub, then resolve it via a multi-component path.
    Inode* subfile = create_or_null(sub, "inner", 5);
    TEST_ASSERT_NOT_NULL(subfile);
    Inode* resolved = lookup_or_null(&tfs, "sub/inner");
    TEST_ASSERT_NOT_NULL(resolved);
    TEST_ASSERT_EQ(resolved, subfile);  // same stable inode

    // readdir on sub lists "inner".
    TEST_ASSERT_EQ(readdir_or_neg1(sub, 2, name, sizeof(name)), 1);
    TEST_ASSERT_EQ(strcmp(name, "inner"), 0);
}

// ---------- stat ----------

void test_stat_file_and_dir() {
    TmpFs tfs;
    TEST_ASSERT_TRUE(tfs.mount().ok());
    Inode* root = lookup_or_null(&tfs, "");

    Inode* f = create_or_null(root, "file", 4);
    TEST_ASSERT_NOT_NULL(f);
    write_or_neg1(f, 0, "0123456789", 10);

    struct stat st;
    TEST_ASSERT_TRUE(f->ops->stat(f, &st).ok());
    TEST_ASSERT_EQ(st.st_mode, static_cast<uint32_t>(kTmpfsSIfReg | 0644));
    TEST_ASSERT_EQ(st.st_size, 10u);
    TEST_ASSERT_EQ(st.st_nlink, 1u);
    TEST_ASSERT_NE(st.st_ino, 0u);  // a real inode number, not the root

    TEST_ASSERT_TRUE(f->ops->chmod(f, 0755).ok());
    TEST_ASSERT_TRUE(f->ops->stat(f, &st).ok());
    TEST_ASSERT_EQ(st.st_mode, static_cast<uint32_t>(kTmpfsSIfReg | 0755));

    Inode* d = mkdir_or_null(root, "dir", 3);
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_TRUE(d->ops->stat(d, &st).ok());
    TEST_ASSERT_EQ(st.st_mode, static_cast<uint32_t>(kTmpfsSIfDir | 0755));
    TEST_ASSERT_EQ(st.st_nlink, 2u);

    TEST_ASSERT_TRUE(d->ops->chmod(d, 0700).ok());
    TEST_ASSERT_TRUE(d->ops->stat(d, &st).ok());
    TEST_ASSERT_EQ(st.st_mode, static_cast<uint32_t>(kTmpfsSIfDir | 0700));
}

// ---------- unlink ----------

void test_unlink_file_then_lookup_fails() {
    TmpFs tfs;
    TEST_ASSERT_TRUE(tfs.mount().ok());
    Inode* root = lookup_or_null(&tfs, "");
    TEST_ASSERT_NOT_NULL(create_or_null(root, "gone", 4));

    TEST_ASSERT_EQ(unlink_rc(root, "gone", 4), 0);
    TEST_ASSERT_NULL(lookup_or_null(&tfs, "gone"));
    // Unlinking a missing name fails.
    TEST_ASSERT_EQ(unlink_rc(root, "gone", 4), -1);
}

void test_unlink_empty_dir_ok_nonempty_fails() {
    TmpFs tfs;
    TEST_ASSERT_TRUE(tfs.mount().ok());
    Inode* root = lookup_or_null(&tfs, "");

    // An empty directory can be removed (the sys_rmdir path).
    TEST_ASSERT_NOT_NULL(mkdir_or_null(root, "empty", 5));
    TEST_ASSERT_EQ(unlink_rc(root, "empty", 5), 0);
    TEST_ASSERT_NULL(lookup_or_null(&tfs, "empty"));

    // A non-empty directory cannot.
    Inode* full = mkdir_or_null(root, "full", 4);
    TEST_ASSERT_NOT_NULL(full);
    TEST_ASSERT_NOT_NULL(create_or_null(full, "child", 5));
    TEST_ASSERT_EQ(unlink_rc(root, "full", 4), -1);
    // Removing the child first makes the directory removable.
    TEST_ASSERT_EQ(unlink_rc(full, "child", 5), 0);
    TEST_ASSERT_EQ(unlink_rc(root, "full", 4), 0);
}

// ---------- negative paths ----------

void test_negative_lookup_duplicate_and_descend_through_file() {
    TmpFs tfs;
    TEST_ASSERT_TRUE(tfs.mount().ok());
    Inode* root = lookup_or_null(&tfs, "");

    TEST_ASSERT_NULL(lookup_or_null(&tfs, "nope"));  // missing entry

    // Duplicate create fails (AlreadyExists).
    TEST_ASSERT_NOT_NULL(create_or_null(root, "x", 1));
    TEST_ASSERT_NULL(create_or_null(root, "x", 1));

    // Descending through a file is NotFound ("x" is a file, "x/anything" bad).
    TEST_ASSERT_NULL(lookup_or_null(&tfs, "x/anything"));
}

}  // namespace test_tmpfs

// ============================================================
// Entry point
// ============================================================

extern "C" void run_tmpfs_tests() {
    TEST_SECTION("TmpFs (F6-M4)");
    RUN_TEST(test_tmpfs::test_mount_and_root);
    RUN_TEST(test_tmpfs::test_create_write_read_roundtrip);
    RUN_TEST(test_tmpfs::test_append_and_offset_read);
    RUN_TEST(test_tmpfs::test_truncate_shrink_and_extend);
    RUN_TEST(test_tmpfs::test_grow_past_4k_boundary_and_gap);
    RUN_TEST(test_tmpfs::test_mkdir_readdir_and_nested_lookup);
    RUN_TEST(test_tmpfs::test_stat_file_and_dir);
    RUN_TEST(test_tmpfs::test_unlink_file_then_lookup_fails);
    RUN_TEST(test_tmpfs::test_unlink_empty_dir_ok_nonempty_fails);
    RUN_TEST(test_tmpfs::test_negative_lookup_duplicate_and_descend_through_file);
    TEST_SUMMARY();
}
