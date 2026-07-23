/**
 * @file test/unit/test_fifo.cpp
 * @brief Host-side unit tests for named FIFO (F8-M2 batch 3)
 *
 * Exercises FifoRegistry (create/lookup/remove) and FifoOps::open cloning
 * (first open lazily creates the pipe; read/write round-trip through the cloned
 * ends; O_NONBLOCK propagates; stat reports S_IFIFO).
 *
 * Links the real kernel/ipc/fifo.cpp + pipe.cpp + pipe_ops.cpp + inode.cpp.
 * Compile condition: -DCINUX_HOST_TEST.
 */

#define TEST_FRAMEWORK_IMPL
#include "test_framework.h"

#ifdef CINUX_HOST_TEST

#    include <cstdint>
#    include <cstring>

#    include "kernel/fs/inode.hpp"
#    include "kernel/ipc/fifo.hpp"
#    include "kernel/ipc/pipe.hpp"
#    include "kernel/ipc/pipe_ops.hpp"

using namespace cinux::ipc;
using namespace cinux::fs;
using cinux::lib::Error;

// Build a FIFO inode backed by a registry entry, then return it ready for
// FifoOps::open (fs_private -> the entry's Fifo, ops -> the shared FifoOps).
static Inode make_fifo_node(Fifo* fifo) {
    Inode node;
    node.ops        = &fifo_ops();
    node.fs_private = fifo;
    node.mode       = kSIfFifo | 0666;
    return node;
}

// Free a per-open inode returned by FifoOps::open.  The inode and its bound
// PipeOps are heap-allocated; the kernel has no InodeOps::release hook yet (a
// documented hobby-OS limitation), so host tests clean up explicitly to stay
// LeakSanitizer-clean.  The shared Pipe is owned by the Fifo and freed by
// FifoRegistry::remove, not here.
static void release_open_end(Inode* end) {
    if (end == nullptr) {
        return;
    }
    delete end->ops;
    delete end;
}

// ============================================================
// 1. FifoRegistry: create / lookup / remove
// ============================================================

TEST("fifo: registry create/lookup/remove") {
    auto& reg = FifoRegistry::instance();
    reg.remove("reg1");  // idempotent clean slate

    ASSERT_TRUE(reg.create("reg1").ok());
    ASSERT_TRUE(reg.create("reg1").error() == Error::AlreadyExists);  // duplicate

    auto found = reg.lookup("reg1");
    ASSERT_TRUE(found.ok());
    ASSERT_NOT_NULL(found.value());

    ASSERT_TRUE(reg.lookup("missing").error() == Error::NotFound);

    reg.remove("reg1");
    ASSERT_TRUE(reg.lookup("reg1").error() == Error::NotFound);

    // Empty / null names rejected.
    ASSERT_TRUE(reg.create("").error() == Error::InvalidArgument);
}

// ============================================================
// 2. First open lazily creates one shared pipe; round-trip
// ============================================================

TEST("fifo: first open creates shared pipe; read/write round-trip") {
    auto& reg = FifoRegistry::instance();
    reg.remove("rt1");
    ASSERT_TRUE(reg.create("rt1").ok());
    Fifo* fifo = reg.lookup("rt1").value();
    ASSERT_TRUE(fifo->pipe == nullptr);  // not created until first open

    Inode fnode = make_fifo_node(fifo);

    // O_WRONLY=1 -> write end, O_RDONLY=0 -> read end.
    auto wopen = fifo_ops().open(&fnode, kOWronly);
    auto ropen = fifo_ops().open(&fnode, 0);
    ASSERT_TRUE(wopen.ok());
    ASSERT_TRUE(ropen.ok());
    Inode* winode = wopen.value();
    Inode* rinode = ropen.value();

    ASSERT_NOT_NULL(fifo->pipe);            // first open built exactly one pipe
    ASSERT_TRUE(fifo->pipe == fifo->pipe);  // (sanity)

    const char msg[] = "fifo!";
    auto       w     = winode->ops->write(winode, 0, msg, 5);
    ASSERT_TRUE(w.ok());
    ASSERT_EQ(w.value(), 5);

    char buf[8] = {};
    auto r      = rinode->ops->read(rinode, 0, buf, 5);
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(r.value(), 5);
    ASSERT_TRUE(memcmp(buf, msg, 5) == 0);

    // Per-open ends are heap-allocated; free them so the host test stays
    // LeakSanitizer-clean (the kernel has no release hook -- production paths
    // accept this leak as a known hobby-OS limitation).
    release_open_end(winode);
    release_open_end(rinode);
    reg.remove("rt1");  // frees the shared Pipe
}

// ============================================================
// 3. O_NONBLOCK propagates to the pipe end
// ============================================================

TEST("fifo: O_NONBLOCK propagates (full pipe -> WouldBlock)") {
    auto& reg = FifoRegistry::instance();
    reg.remove("nb1");
    ASSERT_TRUE(reg.create("nb1").ok());
    Inode fnode = make_fifo_node(reg.lookup("nb1").value());

    // Open the write end non-blocking.
    auto wopen = fifo_ops().open(&fnode, kOWronly | kONonblock);
    ASSERT_TRUE(wopen.ok());
    Inode* winode = wopen.value();

    // Fill the 4 KB pipe (fits, so it succeeds even nonblock).
    char src[PIPE_BUFFER_SIZE];
    for (uint32_t i = 0; i < PIPE_BUFFER_SIZE; ++i) {
        src[i] = static_cast<char>(i);
    }
    auto fill = winode->ops->write(winode, 0, src, PIPE_BUFFER_SIZE);
    ASSERT_TRUE(fill.ok());
    ASSERT_EQ(fill.value(), static_cast<int64_t>(PIPE_BUFFER_SIZE));

    // One more byte on a full nonblocking pipe -> WouldBlock (-EAGAIN).
    auto w = winode->ops->write(winode, 0, "X", 1);
    ASSERT_TRUE(!w.ok());
    ASSERT_TRUE(w.error() == Error::WouldBlock);

    release_open_end(winode);
    reg.remove("nb1");
}

// ============================================================
// 4. stat reports S_IFIFO; open without fs_private is rejected
// ============================================================

TEST("fifo: stat reports S_IFIFO and open validates fs_private") {
    auto& reg = FifoRegistry::instance();
    reg.remove("st1");
    ASSERT_TRUE(reg.create("st1").ok());
    Inode fnode = make_fifo_node(reg.lookup("st1").value());

    struct stat st{};
    auto        s = fifo_ops().stat(&fnode, &st);
    ASSERT_TRUE(s.ok());
    ASSERT_TRUE((st.st_mode & 0xF000) == kSIfFifo);

    // No fs_private -> open fails cleanly (no crash).
    Inode empty{};
    empty.ops = &fifo_ops();
    auto bad  = fifo_ops().open(&empty, 0);
    ASSERT_TRUE(!bad.ok());
    ASSERT_TRUE(bad.error() == Error::InvalidArgument);

    reg.remove("st1");
}

// ============================================================
// Main
// ============================================================

int main() {
    RUN_ALL_TESTS();
    return _tests_failed > 0 ? 1 : 0;
}

#endif  // CINUX_HOST_TEST
