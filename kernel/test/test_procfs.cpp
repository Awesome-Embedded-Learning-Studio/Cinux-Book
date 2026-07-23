/**
 * @file kernel/test/test_procfs.cpp
 * @brief In-kernel tests for ProcFS (F6-M2)
 *
 * Runs inside QEMU via run-kernel-test.  procfs.cpp reads the live task
 * registry (no host injection seam), so ProcFS is exercised here under the
 * freestanding build rather than in test/unit.  A stack Task is registered with
 * a known free PID so the lookup-found / lookup-NotFound assertions are
 * deterministic regardless of tasks left behind by earlier test sections
 * (mirrors the F3-M4 stack-Task technique).
 *
 * Batch 1: mount, root + per-PID directory lookup, directory stat, root readdir
 * (dot/dotdot only).  Later batches extend this file (readdir PID enumeration,
 * stat/cmdline pseudo-file reads).
 */

#include <stdint.h>

#include "kernel/fs/procfs.hpp"
#include "kernel/lib/string.hpp"    // utoa
#include "kernel/proc/process.hpp"  // Task
#include "kernel/proc/signal.hpp"   // signal_register/unregister/find_by_pid
#include "kernel/test/big_kernel_test.h"

using namespace cinux::fs;
using cinux::lib::kprintf;
using cinux::proc::Task;
using cinux::proc::signal_find_task_by_pid;
using cinux::proc::signal_register_task;
using cinux::proc::signal_unregister_task;

namespace {

/// Pick a PID in [200, 256] that is currently free in the registry.  The test
/// kernel's own main thread is not registered (it runs on the boot stack), but
/// earlier sections may have left tasks behind; starting from a high band keeps
/// the lookup-found / NotFound assertions deterministic without assuming an
/// empty registry.
int pick_free_pid() {
    for (int p = 200; p <= kProcPidMax; ++p) {
        if (signal_find_task_by_pid(p) == nullptr) {
            return p;
        }
    }
    return 200;
}

/// Write the decimal of @p pid into @p buf (enough for any value <= 256).
void pid_to_path(char* buf, int pid) {
    utoa(buf, static_cast<uint32_t>(pid));
}

/// Substring search (kernel lib has no strstr).  @p hay_len lets the caller pass
/// a buffer that is not NUL-terminated (procfs reads return raw bytes).
bool contains(const char* hay, int hay_len, const char* needle) {
    int nl = static_cast<int>(strlen(needle));
    for (int i = 0; i + nl <= hay_len; ++i) {
        if (memcmp(hay + i, needle, static_cast<uint32_t>(nl)) == 0) {
            return true;
        }
    }
    return false;
}

}  // namespace

namespace test_procfs {

// ---------- mount + root ----------

void test_mount_and_root() {
    ProcFs pf;
    TEST_ASSERT_TRUE(pf.mount().ok());
    Inode* root = lookup_or_null(&pf, "");
    TEST_ASSERT_NOT_NULL(root);
    TEST_ASSERT_EQ(static_cast<int>(root->type), static_cast<int>(InodeType::Directory));
    // mount() is idempotent (mirrors DevFs).
    TEST_ASSERT_TRUE(pf.mount().ok());
}

void test_root_stat_is_directory() {
    ProcFs pf;
    TEST_ASSERT_TRUE(pf.mount().ok());
    Inode*      root = lookup_or_null(&pf, "");
    struct stat st;
    TEST_ASSERT_TRUE(root->ops->stat(root, &st).ok());
    TEST_ASSERT_EQ(st.st_mode, static_cast<uint32_t>(kProcSIfDir | 0555));
    TEST_ASSERT_EQ(st.st_nlink, 2u);
}

void test_root_readdir_dot_dotdot() {
    ProcFs pf;
    TEST_ASSERT_TRUE(pf.mount().ok());
    Inode* root = lookup_or_null(&pf, "");
    char   name[PROCFS_NAME_MAX];

    TEST_ASSERT_EQ(readdir_or_neg1(root, 0, name, sizeof(name)), 1);
    TEST_ASSERT_EQ(name[0], '.');
    TEST_ASSERT_EQ(name[1], '\0');

    TEST_ASSERT_EQ(readdir_or_neg1(root, 1, name, sizeof(name)), 1);
    TEST_ASSERT_EQ(name[0], '.');
    TEST_ASSERT_EQ(name[1], '.');
}

void test_root_readdir_lists_live_pid() {
    ProcFs pf;
    TEST_ASSERT_TRUE(pf.mount().ok());

    int  pid = pick_free_pid();
    Task t{};
    t.pid  = pid;
    t.name = "procfs_test";
    signal_register_task(&t);

    Inode* root = lookup_or_null(&pf, "");
    char   name[PROCFS_NAME_MAX];
    char   expected[16];
    pid_to_path(expected, pid);

    // Walk past "." / ".." (indices 0/1) and find our PID among the live set.
    bool found = false;
    for (uint64_t idx = 2; idx < 64; ++idx) {
        int64_t r = readdir_or_neg1(root, idx, name, sizeof(name));
        if (r != 1) {
            break;  // end of directory
        }
        if (strcmp(name, expected) == 0) {
            found = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(found);

    // After unregister, the PID no longer appears.
    signal_unregister_task(&t);
    found = false;
    for (uint64_t idx = 2; idx < 64; ++idx) {
        int64_t r = readdir_or_neg1(root, idx, name, sizeof(name));
        if (r != 1) {
            break;
        }
        if (strcmp(name, expected) == 0) {
            found = true;
            break;
        }
    }
    TEST_ASSERT_FALSE(found);
}

// ---------- per-PID directory lookup ----------

void test_lookup_pid_dir_live_then_dead() {
    ProcFs pf;
    TEST_ASSERT_TRUE(pf.mount().ok());

    int  pid = pick_free_pid();
    Task t{};
    t.pid  = pid;
    t.name = "procfs_test";
    signal_register_task(&t);

    char path[16];
    pid_to_path(path, pid);

    // A live PID resolves to a per-process directory.
    Inode* dir = lookup_or_null(&pf, path);
    TEST_ASSERT_NOT_NULL(dir);
    TEST_ASSERT_EQ(static_cast<int>(dir->type), static_cast<int>(InodeType::Directory));

    struct stat st;
    TEST_ASSERT_TRUE(dir->ops->stat(dir, &st).ok());
    TEST_ASSERT_EQ(st.st_mode, static_cast<uint32_t>(kProcSIfDir | 0555));
    TEST_ASSERT_EQ(st.st_ino, static_cast<uint64_t>(pid));

    // Once unregistered, the same PID is gone (Linux only exposes live tasks).
    signal_unregister_task(&t);
    TEST_ASSERT_NULL(lookup_or_null(&pf, path));
}

void test_lookup_rejects_bad_paths() {
    ProcFs pf;
    TEST_ASSERT_TRUE(pf.mount().ok());

    TEST_ASSERT_NULL(lookup_or_null(&pf, "notapid"));  // non-numeric
    TEST_ASSERT_NULL(lookup_or_null(&pf, "0"));        // PID 0 is invalid
    TEST_ASSERT_NULL(lookup_or_null(&pf, "99999"));    // out of the allocator range
    TEST_ASSERT_NULL(lookup_or_null(&pf, nullptr));    // null path
}

// ---------- /proc/<pid>/ pseudo-files ----------

/// Register a stack task with a known free PID + name; return it via @p out so
/// the caller can unregister.  Keeps the per-file tests DRY.
int register_named_task(Task* out) {
    int pid   = pick_free_pid();
    out->pid  = pid;
    out->name = "procfs_test";
    out->ppid = 1;
    out->tgid = pid;
    out->uid  = 0;
    out->gid  = 0;
    signal_register_task(out);
    return pid;
}

void test_pid_dir_readdir_lists_stat_and_cmdline() {
    ProcFs pf;
    TEST_ASSERT_TRUE(pf.mount().ok());

    Task t{};
    int  pid = register_named_task(&t);
    char dir_path[16];
    pid_to_path(dir_path, pid);

    Inode* dir = lookup_or_null(&pf, dir_path);
    TEST_ASSERT_NOT_NULL(dir);
    char name[PROCFS_NAME_MAX];

    TEST_ASSERT_EQ(readdir_or_neg1(dir, 0, name, sizeof(name)), 1);  // "."
    TEST_ASSERT_EQ(readdir_or_neg1(dir, 1, name, sizeof(name)), 1);  // ".."
    TEST_ASSERT_EQ(readdir_or_neg1(dir, 2, name, sizeof(name)), 1);
    TEST_ASSERT_EQ(strcmp(name, "stat"), 0);
    TEST_ASSERT_EQ(readdir_or_neg1(dir, 3, name, sizeof(name)), 1);
    TEST_ASSERT_EQ(strcmp(name, "cmdline"), 0);
    TEST_ASSERT_EQ(readdir_or_neg1(dir, 4, name, sizeof(name)), 0);  // end

    signal_unregister_task(&t);
}

void test_lookup_resolves_pseudo_files() {
    ProcFs pf;
    TEST_ASSERT_TRUE(pf.mount().ok());

    Task t{};
    int  pid = register_named_task(&t);
    char stat_path[24];
    char cmd_path[24];
    char bogus_path[24];
    // build "<pid>/stat" etc.
    {
        char pid_s[16];
        pid_to_path(pid_s, pid);
        strcpy(stat_path, pid_s);
        strcpy(stat_path + strlen(pid_s), "/stat");
        strcpy(cmd_path, pid_s);
        strcpy(cmd_path + strlen(pid_s), "/cmdline");
        strcpy(bogus_path, pid_s);
        strcpy(bogus_path + strlen(pid_s), "/bogus");
    }

    Inode* st = lookup_or_null(&pf, stat_path);
    TEST_ASSERT_NOT_NULL(st);
    TEST_ASSERT_EQ(static_cast<int>(st->type), static_cast<int>(InodeType::Regular));
    struct stat sst;
    TEST_ASSERT_TRUE(st->ops->stat(st, &sst).ok());
    TEST_ASSERT_EQ(sst.st_mode, static_cast<uint32_t>(kProcSIfReg | 0444));

    Inode* cm = lookup_or_null(&pf, cmd_path);
    TEST_ASSERT_NOT_NULL(cm);

    TEST_ASSERT_NULL(lookup_or_null(&pf, bogus_path));  // unknown leaf

    signal_unregister_task(&t);
}

void test_stat_read_contains_pid_and_name() {
    ProcFs pf;
    TEST_ASSERT_TRUE(pf.mount().ok());

    Task t{};
    int  pid = register_named_task(&t);
    char stat_path[24];
    {
        char pid_s[16];
        pid_to_path(pid_s, pid);
        strcpy(stat_path, pid_s);
        strcpy(stat_path + strlen(pid_s), "/stat");
    }

    Inode* st = lookup_or_null(&pf, stat_path);
    TEST_ASSERT_NOT_NULL(st);
    char    buf[128];
    int64_t n = read_or_neg1(st, 0, buf, sizeof(buf) - 1);
    TEST_ASSERT_GT(n, 0);
    buf[n] = '\0';

    char pid_s[16];
    pid_to_path(pid_s, pid);
    TEST_ASSERT_TRUE(contains(buf, static_cast<int>(n), pid_s));             // pid present
    TEST_ASSERT_TRUE(contains(buf, static_cast<int>(n), "procfs_test"));     // name present
    TEST_ASSERT_TRUE(contains(buf, static_cast<int>(n), "(procfs_test) "));  // "(name) " layout

    signal_unregister_task(&t);
}

void test_cmdline_read_contains_name() {
    ProcFs pf;
    TEST_ASSERT_TRUE(pf.mount().ok());

    Task t{};
    int  pid = register_named_task(&t);
    char cmd_path[24];
    {
        char pid_s[16];
        pid_to_path(pid_s, pid);
        strcpy(cmd_path, pid_s);
        strcpy(cmd_path + strlen(pid_s), "/cmdline");
    }

    Inode* cm = lookup_or_null(&pf, cmd_path);
    TEST_ASSERT_NOT_NULL(cm);
    char    buf[64];
    int64_t n = read_or_neg1(cm, 0, buf, sizeof(buf));
    TEST_ASSERT_GT(n, 0);
    TEST_ASSERT_TRUE(contains(buf, static_cast<int>(n), "procfs_test"));

    signal_unregister_task(&t);
}

void test_stat_read_dead_pid_is_not_found() {
    ProcFs pf;
    TEST_ASSERT_TRUE(pf.mount().ok());

    Task t{};
    int  pid = register_named_task(&t);
    char stat_path[24];
    {
        char pid_s[16];
        pid_to_path(pid_s, pid);
        strcpy(stat_path, pid_s);
        strcpy(stat_path + strlen(pid_s), "/stat");
    }

    Inode* st = lookup_or_null(&pf, stat_path);
    TEST_ASSERT_NOT_NULL(st);
    char buf[64];
    TEST_ASSERT_GT(read_or_neg1(st, 0, buf, sizeof(buf)), 0);  // live: reads

    // After unregister the task is gone; reading the same inode now fails
    // (registry TOCTOU window -- documented behaviour).
    signal_unregister_task(&t);
    TEST_ASSERT_EQ(read_or_neg1(st, 0, buf, sizeof(buf)), -1);
}

}  // namespace test_procfs

// ============================================================
// Entry point
// ============================================================

extern "C" void run_procfs_tests() {
    TEST_SECTION("ProcFS (F6-M2)");
    RUN_TEST(test_procfs::test_mount_and_root);
    RUN_TEST(test_procfs::test_root_stat_is_directory);
    RUN_TEST(test_procfs::test_root_readdir_dot_dotdot);
    RUN_TEST(test_procfs::test_root_readdir_lists_live_pid);
    RUN_TEST(test_procfs::test_lookup_pid_dir_live_then_dead);
    RUN_TEST(test_procfs::test_lookup_rejects_bad_paths);
    RUN_TEST(test_procfs::test_pid_dir_readdir_lists_stat_and_cmdline);
    RUN_TEST(test_procfs::test_lookup_resolves_pseudo_files);
    RUN_TEST(test_procfs::test_stat_read_contains_pid_and_name);
    RUN_TEST(test_procfs::test_cmdline_read_contains_name);
    RUN_TEST(test_procfs::test_stat_read_dead_pid_is_not_found);
    TEST_SUMMARY();
}
