/**
 * @file test/unit/test_devfs.cpp
 * @brief Host-side unit tests for DevFS (F6-M3)
 *
 * Links the real kernel/fs/devfs.cpp -- which is pure logic (no kprintf, no
 * kernel-only I/O) -- and exercises every device node.  /dev/console writes
 * go through a capturing MockSink, proving the CharSink dispatch is correct
 * without touching real hardware.
 *
 * Compile condition: -DCINUX_HOST_TEST
 */

#define TEST_FRAMEWORK_IMPL
#include "test_framework.h"

#ifdef CINUX_HOST_TEST

#    include <cstdint>
#    include <cstring>

#    include "fs/devfs/devfs.hpp"

using namespace cinux::fs;

// ============================================================
// MockSink -- captures bytes written through a CharSink
// ============================================================

class MockSink : public CharSink {
public:
    cinux::lib::ErrorOr<int64_t> write(const void* buf, uint64_t count) override {
        const auto* b = static_cast<const uint8_t*>(buf);
        uint64_t    n = 0;
        for (; n < count && len_ < sizeof(buf_); ++n, ++len_) {
            buf_[len_] = b[n];
        }
        return static_cast<int64_t>(n);
    }

    uint8_t  buf_[256]{};
    uint64_t len_{0};
};

// ============================================================
// 1. mount -- standard nodes registered
// ============================================================

TEST("devfs: mount registers null/zero/console") {
    DevFs devfs(nullptr);
    ASSERT_TRUE(devfs.mount().ok());
    ASSERT_EQ(devfs.node_count(), 3u);
}

TEST("devfs: mount is idempotent (re-mount does not duplicate)") {
    DevFs devfs(nullptr);
    ASSERT_TRUE(devfs.mount().ok());
    ASSERT_TRUE(devfs.mount().ok());
    // Re-mount re-allocates ops but the node table stays bounded at 3.
    ASSERT_EQ(devfs.node_count(), 3u);
}

// ============================================================
// 2. lookup -- nodes, root, missing
// ============================================================

TEST("devfs: lookup finds each device node") {
    DevFs devfs(nullptr);
    ASSERT_TRUE(devfs.mount().ok());
    ASSERT_NOT_NULL(devfs.lookup("null").value());
    ASSERT_NOT_NULL(devfs.lookup("zero").value());
    ASSERT_NOT_NULL(devfs.lookup("console").value());
}

TEST("devfs: lookup accepts a leading slash") {
    DevFs devfs(nullptr);
    ASSERT_TRUE(devfs.mount().ok());
    // vfs_resolve may pass "/null" (mount prefix without trailing slash).
    ASSERT_NOT_NULL(devfs.lookup("/null").value());
}

TEST("devfs: lookup empty or slash returns the root directory") {
    DevFs devfs(nullptr);
    ASSERT_TRUE(devfs.mount().ok());
    Inode* a = devfs.lookup("").value();
    Inode* b = devfs.lookup("/").value();
    ASSERT_NOT_NULL(a);
    ASSERT_TRUE(a == b);  // same root inode
    ASSERT_EQ(static_cast<int>(a->type), static_cast<int>(InodeType::Directory));
}

TEST("devfs: lookup unknown name returns NotFound") {
    DevFs devfs(nullptr);
    ASSERT_TRUE(devfs.mount().ok());
    auto r = devfs.lookup("nope");
    ASSERT_FALSE(r.ok());
}

TEST("devfs: lookup nullptr is InvalidArgument") {
    DevFs devfs(nullptr);
    ASSERT_TRUE(devfs.mount().ok());
    auto r = devfs.lookup(nullptr);
    ASSERT_FALSE(r.ok());
}

// ============================================================
// 3. /dev/null -- read EOF, write discards
// ============================================================

TEST("devfs: /dev/null read returns EOF") {
    DevFs devfs(nullptr);
    ASSERT_TRUE(devfs.mount().ok());
    Inode* n = devfs.lookup("null").value();
    char   buf[8];
    auto   r = n->ops->read(n, 0, buf, sizeof(buf));
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(r.value(), 0);
}

TEST("devfs: /dev/null write discards and claims all bytes") {
    DevFs devfs(nullptr);
    ASSERT_TRUE(devfs.mount().ok());
    Inode*     n     = devfs.lookup("null").value();
    const char msg[] = "discard me";
    auto       w     = n->ops->write(n, 0, msg, sizeof(msg) - 1);
    ASSERT_TRUE(w.ok());
    ASSERT_EQ(w.value(), static_cast<int64_t>(sizeof(msg) - 1));
}

// ============================================================
// 4. /dev/zero -- read yields zeros, write discards
// ============================================================

TEST("devfs: /dev/zero read fills buffer with zeros") {
    DevFs devfs(nullptr);
    ASSERT_TRUE(devfs.mount().ok());
    Inode*        z = devfs.lookup("zero").value();
    unsigned char buf[16];
    std::memset(buf, 0xFF, sizeof(buf));
    auto r = z->ops->read(z, 0, buf, 8);
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(r.value(), 8);
    for (int i = 0; i < 8; ++i) {
        ASSERT_EQ(static_cast<int>(buf[i]), 0);
    }
    // Beyond the requested count the buffer is untouched.
    ASSERT_EQ(static_cast<int>(buf[8]), static_cast<int>(0xFF));
}

TEST("devfs: /dev/zero write discards") {
    DevFs devfs(nullptr);
    ASSERT_TRUE(devfs.mount().ok());
    Inode*     z     = devfs.lookup("zero").value();
    const char msg[] = "x";
    auto       w     = z->ops->write(z, 0, msg, 1);
    ASSERT_TRUE(w.ok());
    ASSERT_EQ(w.value(), 1);
}

// ============================================================
// 5. /dev/console -- writes route through the sink; read unsupported
// ============================================================

TEST("devfs: /dev/console write routes bytes through the sink") {
    MockSink sink;
    DevFs    devfs(&sink);
    ASSERT_TRUE(devfs.mount().ok());
    Inode*     con   = devfs.lookup("console").value();
    const char msg[] = "hi";
    auto       w     = con->ops->write(con, 0, msg, 2);
    ASSERT_TRUE(w.ok());
    ASSERT_EQ(w.value(), 2);
    ASSERT_EQ(sink.len_, 2u);
    ASSERT_EQ(static_cast<int>(sink.buf_[0]), 'h');
    ASSERT_EQ(static_cast<int>(sink.buf_[1]), 'i');
}

TEST("devfs: /dev/console write with no sink discards") {
    DevFs devfs(nullptr);  // no sink
    ASSERT_TRUE(devfs.mount().ok());
    Inode*     con   = devfs.lookup("console").value();
    const char msg[] = "x";
    auto       w     = con->ops->write(con, 0, msg, 1);
    ASSERT_TRUE(w.ok());
    ASSERT_EQ(w.value(), 1);
}

TEST("devfs: /dev/console read is unsupported") {
    DevFs devfs(nullptr);
    ASSERT_TRUE(devfs.mount().ok());
    Inode* con = devfs.lookup("console").value();
    char   buf[4];
    auto   r = con->ops->read(con, 0, buf, sizeof(buf));
    ASSERT_FALSE(r.ok());
}

// ============================================================
// 6. readdir -- root directory lists . .. null zero console
// ============================================================

TEST("devfs: readdir lists dot, dotdot, then nodes in order") {
    DevFs devfs(nullptr);
    ASSERT_TRUE(devfs.mount().ok());
    Inode* root = devfs.lookup("").value();

    char name[DEVFS_NAME_MAX];

    auto e0 = root->ops->readdir(root, 0, name, sizeof(name));
    ASSERT_TRUE(e0.ok());
    ASSERT_EQ(e0.value(), 1);
    ASSERT_EQ(std::strcmp(name, "."), 0);

    auto e1 = root->ops->readdir(root, 1, name, sizeof(name));
    ASSERT_TRUE(e1.ok());
    ASSERT_EQ(e1.value(), 1);
    ASSERT_EQ(std::strcmp(name, ".."), 0);

    auto e2 = root->ops->readdir(root, 2, name, sizeof(name));
    ASSERT_TRUE(e2.ok());
    ASSERT_EQ(e2.value(), 1);
    ASSERT_EQ(std::strcmp(name, "null"), 0);

    auto e3 = root->ops->readdir(root, 3, name, sizeof(name));
    ASSERT_TRUE(e3.ok());
    ASSERT_EQ(e3.value(), 1);
    ASSERT_EQ(std::strcmp(name, "zero"), 0);

    auto e4 = root->ops->readdir(root, 4, name, sizeof(name));
    ASSERT_TRUE(e4.ok());
    ASSERT_EQ(e4.value(), 1);
    ASSERT_EQ(std::strcmp(name, "console"), 0);

    // Past the last node readdir signals end-of-directory (0).
    auto e5 = root->ops->readdir(root, 5, name, sizeof(name));
    ASSERT_TRUE(e5.ok());
    ASSERT_EQ(e5.value(), 0);
}

// ============================================================
// 7. stat -- device nodes report S_IFCHR + distinguishable st_rdev
// ============================================================

TEST("devfs: null stat reports character device 1:3") {
    DevFs devfs(nullptr);
    ASSERT_TRUE(devfs.mount().ok());
    Inode*      n = devfs.lookup("null").value();
    struct stat st;
    auto        r = n->ops->stat(n, &st);
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(st.st_mode, static_cast<uint32_t>(kSIfChr | 0666));
    ASSERT_EQ(st.st_rdev, devfs_makedev(1, 3));
    ASSERT_EQ(st.st_nlink, 1u);
}

TEST("devfs: zero stat reports character device 1:5") {
    DevFs devfs(nullptr);
    ASSERT_TRUE(devfs.mount().ok());
    Inode*      z = devfs.lookup("zero").value();
    struct stat st;
    ASSERT_TRUE(z->ops->stat(z, &st).ok());
    ASSERT_EQ(st.st_rdev, devfs_makedev(1, 5));
}

TEST("devfs: console stat reports character device 5:1") {
    DevFs devfs(nullptr);
    ASSERT_TRUE(devfs.mount().ok());
    Inode*      c = devfs.lookup("console").value();
    struct stat st;
    ASSERT_TRUE(c->ops->stat(c, &st).ok());
    ASSERT_EQ(st.st_rdev, devfs_makedev(5, 1));
}

TEST("devfs: root stat reports a directory") {
    DevFs devfs(nullptr);
    ASSERT_TRUE(devfs.mount().ok());
    Inode*      root = devfs.lookup("").value();
    struct stat st;
    ASSERT_TRUE(root->ops->stat(root, &st).ok());
    ASSERT_EQ(st.st_mode, static_cast<uint32_t>(kSIfDir | 0755));
    ASSERT_EQ(st.st_nlink, 2u);
}

// ============================================================
// Main
// ============================================================

int main() {
    RUN_ALL_TESTS();
    return _tests_failed > 0 ? 1 : 0;
}

#endif  // CINUX_HOST_TEST
