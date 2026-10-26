#include <atomic>
#include <thread>

#include "../framework/framework.hpp"
#include "cinux/result.hpp"
#include "kernel/fs/file_table.hpp"
#include "kernel/fs/vfs.hpp"
#include "test/mock/file_refs.hpp"
#include "test_assert.hpp"

namespace {

cinux::fs::FsNode sample_node(unsigned long ino) {
    return cinux::fs::FsNode{
        .opaque = nullptr,
        .type   = cinux::fs::InodeType::kFile,
        .ino    = ino,
        .size   = 0,
    };
}

}  // namespace

TEST("file table: a born-empty table's first open takes seat zero") {
    cinux::fs::FileTable table;
    const auto           kFirst = table.alloc(nullptr, nullptr, sample_node(1));
    ASSERT_TRUE(kFirst.ok());
    ASSERT_EQ(kFirst.value(), 0UL);
}

TEST("file table: consecutive allocations climb, freed numbers are reused lowest-first") {
    cinux::fs::FileTable table;
    const auto           kOne = table.alloc(nullptr, nullptr, sample_node(1));
    const auto           kTwo = table.alloc(nullptr, nullptr, sample_node(2));
    ASSERT_EQ(kOne.value() + 1, kTwo.value());
    ASSERT_TRUE(table.free(kOne.value()).ok());
    const auto kAgain = table.alloc(nullptr, nullptr, sample_node(3));
    ASSERT_EQ(kAgain.value(), kOne.value());
}

TEST("file table: a full table reports it has no room") {
    cinux::fs::FileTable table;
    for (unsigned long fd = 0; fd < 128UL; ++fd) {
        ASSERT_TRUE(table.alloc(nullptr, nullptr, sample_node(fd), {.limit = 128}).ok());
    }
    const auto kOverflow = table.alloc(nullptr, nullptr, sample_node(99), {.limit = 128});
    ASSERT_FALSE(kOverflow.ok());
    ASSERT_EQ(kOverflow.error(), cinux::base::KernelError::kTooManyOpenFiles);
}

TEST("file table: out-of-range and free slots are refused") {
    cinux::fs::FileTable table;
    const auto           kRange = table.get(cinux::fs::kFileTableMax);
    const auto           kEmpty = table.get(4);
    ASSERT_EQ(kRange.error(), cinux::base::KernelError::kInvalidArgument);
    ASSERT_EQ(kEmpty.error(), cinux::base::KernelError::kNotFound);

    const auto kFreedRange = table.free(cinux::fs::kFileTableMax + 4);
    ASSERT_EQ(kFreedRange.error(), cinux::base::KernelError::kInvalidArgument);
}

TEST("file table: a slot carries its node and its advancing offset") {
    cinux::fs::FileTable table;
    ASSERT_TRUE(table.alloc(nullptr, nullptr, sample_node(7)).ok());
    const auto kSlot = table.get(0);
    ASSERT_TRUE(kSlot.ok());
    ASSERT_EQ(kSlot.value()->node.ino, 7UL);
    ASSERT_EQ(kSlot.value()->offset, 0UL);
    kSlot.value()->offset += 12;
    const auto kReread = table.get(0);
    ASSERT_EQ(kReread.value()->offset, 12UL);
    ASSERT_TRUE(table.free(0).ok());
    const auto kAfter = table.get(0);
    ASSERT_EQ(kAfter.error(), cinux::base::KernelError::kNotFound);
}

TEST("file table: closing a descriptor releases its reference exactly once") {
    cinux::test::FileRefs refs;
    cinux::fs::FileTable  table;
    const auto            kFd = table.alloc(&cinux::test::FileRefs::kOps, &refs, sample_node(1));
    ASSERT_TRUE(kFd.ok());
    ASSERT_EQ(refs.retained, 1UL);
    ASSERT_TRUE(table.free(kFd.value()).ok());
    ASSERT_FALSE(table.free(kFd.value()).ok());
    ASSERT_EQ(refs.released, 1UL);
}

TEST("file table: allocation failure does not retain or release a node") {
    cinux::test::FileRefs refs;
    cinux::fs::FileTable  table;
    for (unsigned long fd = 0; fd < 128UL; ++fd) {
        ASSERT_TRUE(
            table.alloc(&cinux::test::FileRefs::kOps, &refs, sample_node(fd), {.limit = 128}).ok());
    }
    const auto kBefore   = refs.retained;
    const auto kReleased = refs.released;
    ASSERT_FALSE(
        table.alloc(&cinux::test::FileRefs::kOps, &refs, sample_node(99), {.limit = 128}).ok());
    ASSERT_EQ(refs.retained, kBefore);
    ASSERT_EQ(refs.released, kReleased);
}

TEST("file table: destruction closes descriptors left open") {
    cinux::test::FileRefs refs;
    {
        cinux::fs::FileTable table;
        ASSERT_TRUE(table.alloc(&cinux::test::FileRefs::kOps, &refs, sample_node(1)).ok());
        ASSERT_TRUE(table.alloc(&cinux::test::FileRefs::kOps, &refs, sample_node(2)).ok());
    }
    ASSERT_EQ(refs.retained, 2UL);
    ASSERT_EQ(refs.released, 2UL);
}

TEST("file table: close_all is harmless after a previous close_all") {
    cinux::test::FileRefs refs;
    cinux::fs::FileTable  table;
    ASSERT_TRUE(table.alloc(&cinux::test::FileRefs::kOps, &refs, sample_node(1)).ok());
    table.close_all();
    table.close_all();
    ASSERT_EQ(refs.released, 1UL);
}

TEST("file table: an adopted open is shared, its cursor moves for both") {
    cinux::test::FileRefs refs;
    cinux::fs::FileTable  parent;
    cinux::fs::FileTable  child;
    const auto            kFd   = parent.alloc(&cinux::test::FileRefs::kOps, &refs, sample_node(1));
    const auto            kHeld = parent.shared(kFd.value());
    const auto            kChild = child.adopt(kHeld.value());
    ASSERT_TRUE(kChild.ok());
    parent.get(kFd.value()).value()->offset = 40;
    ASSERT_EQ(child.get(kChild.value()).value()->offset, 40UL);
    ASSERT_EQ(refs.retained, 1UL);
}

TEST("file table: a table's free leaves the adopted owner alive") {
    cinux::test::FileRefs refs;
    cinux::fs::FileTable  parent;
    cinux::fs::FileTable  child;
    unsigned long         parent_fd = 0;
    unsigned long         child_fd  = 0;
    {
        const auto kFd    = parent.alloc(&cinux::test::FileRefs::kOps, &refs, sample_node(1));
        const auto kHeld  = parent.shared(kFd.value());
        const auto kChild = child.adopt(kHeld.value());
        ASSERT_TRUE(kChild.ok());
        parent_fd = kFd.value();
        child_fd  = kChild.value();
    }
    ASSERT_TRUE(parent.free(parent_fd).ok());
    ASSERT_EQ(refs.released, 0UL);
    ASSERT_TRUE(child.get(child_fd).ok());
    ASSERT_TRUE(child.free(child_fd).ok());
    ASSERT_EQ(refs.released, 1UL);
}

TEST("file table: closing a cloexec seat clears its flag before ordinary dup reuses it") {
    cinux::fs::FileTable table;
    ASSERT_TRUE(table.alloc(nullptr, nullptr, sample_node(1)).ok());
    const auto kHeld = table.shared(0);
    ASSERT_TRUE(table.adopt_from(kHeld.value(), 10).ok());
    ASSERT_TRUE(table.set_cloexec(10, true).ok());
    ASSERT_TRUE(table.free(10).ok());
    ASSERT_FALSE(table.cloexec(10));
    const auto kReused = table.adopt_from(kHeld.value(), 10);
    ASSERT_EQ(kReused.value(), 10UL);
    table.close_on_exec();
    ASSERT_TRUE(table.get(10).ok());
}

TEST("file table: an acquired open survives concurrent close and descriptor reuse") {
    cinux::test::FileRefs refs;
    cinux::fs::FileTable  table;
    ASSERT_TRUE(table.alloc(&cinux::test::FileRefs::kOps, &refs, sample_node(41)).ok());
    {
        const auto kHeld = table.get(0);
        ASSERT_TRUE(table.free(0).ok());
        ASSERT_EQ(refs.released, 0UL);
        ASSERT_TRUE(table.alloc(nullptr, nullptr, sample_node(42)).ok());
        ASSERT_EQ(kHeld.value()->node.ino, 41UL);
        ASSERT_EQ(table.shared(0).value()->node.ino, 42UL);
    }
    ASSERT_EQ(refs.released, 1UL);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) a body walks one story in order
TEST("file table: dup2 validates its source before replacing the target") {
    cinux::fs::FileTable table;
    ASSERT_TRUE(table.alloc(nullptr, nullptr, sample_node(1)).ok());
    ASSERT_TRUE(table.alloc(nullptr, nullptr, sample_node(2)).ok());
    ASSERT_TRUE(table.set_cloexec(1, true).ok());
    ASSERT_FALSE(table.duplicate_at(9, 1).ok());
    ASSERT_EQ(table.shared(1).value()->node.ino, 2UL);
    ASSERT_TRUE(table.cloexec(1));
    ASSERT_TRUE(table.duplicate_at(0, 1).ok());
    ASSERT_FALSE(table.cloexec(1));
    ASSERT_TRUE(table.shared(0).value().get() == table.shared(1).value().get());
}

TEST("file table: backend release may reenter the table after close") {
    cinux::fs::FileTable table;
    cinux::fs::FsOps     ops{};
    ops.retain  = [](void*, cinux::fs::FsNode) {};
    ops.release = [](void* backend, cinux::fs::FsNode) {
        auto& owner = *static_cast<cinux::fs::FileTable*>(backend);
        ASSERT_TRUE(owner.alloc(nullptr, nullptr, sample_node(19)).ok());
    };
    ASSERT_TRUE(table.alloc(&ops, &table, sample_node(1)).ok());
    ASSERT_TRUE(table.free(0).ok());
    ASSERT_EQ(table.shared(0).value()->node.ino, 19UL);
}

TEST("file table: fork snapshots live opens and descriptor flags together") {
    cinux::fs::FileTable parent;
    ASSERT_TRUE(parent.alloc(nullptr, nullptr, sample_node(3)).ok());
    ASSERT_TRUE(parent.set_cloexec(0, true).ok());
    const auto kChild = parent.clone();
    ASSERT_TRUE(kChild.holds());
    ASSERT_TRUE(parent.free(0).ok());
    ASSERT_EQ(kChild->shared(0).value()->node.ino, 3UL);
    ASSERT_TRUE(kChild->cloexec(0));
    kChild->close_on_exec();
    ASSERT_FALSE(kChild->shared(0).ok());
}

TEST("file table: two allocators and a lookup reader share slots safely") {
    cinux::fs::FileTable  table;
    std::atomic<bool>     start{false};
    std::atomic<unsigned> done{0};
    std::atomic<unsigned> errors{0};
    const auto            kWriter = [&](unsigned long identity) {
        while (!start.load(std::memory_order_acquire)) {
        }
        for (unsigned round = 0; round < 2000; ++round) {
            const auto kFd = table.alloc(nullptr, nullptr, sample_node(identity));
            if (!kFd.ok()) {
                ++errors;
                continue;
            }
            const auto kHeld = table.shared(kFd.value());
            if (!kHeld.ok() || kHeld.value()->node.ino != identity) {
                ++errors;
            }
            if (!table.free(kFd.value()).ok()) {
                ++errors;
            }
        }
        done.fetch_add(1, std::memory_order_release);
    };
    std::thread one(kWriter, 1UL);
    std::thread two(kWriter, 2UL);
    start.store(true, std::memory_order_release);
    while (done.load(std::memory_order_acquire) != 2) {
        for (unsigned long fd = 0; fd < table.capacity(); ++fd) {
            const auto kHeld = table.shared(fd);
            if (kHeld.ok() && (kHeld.value()->node.ino < 1 || kHeld.value()->node.ino > 2)) {
                ++errors;
            }
        }
    }
    one.join();
    two.join();
    ASSERT_EQ(errors.load(), 0U);
}

namespace {

void fill_dynamic_descriptors(cinux::fs::FileTable& table) {
    for (unsigned long descriptor = 0; descriptor < 300; ++descriptor) {
        const auto kFd = table.alloc(nullptr, nullptr, sample_node(descriptor), {.limit = 300});
        ASSERT_TRUE(kFd.ok());
        ASSERT_EQ(kFd.value(), descriptor);
    }
}

}

TEST("file table: dynamic growth and lowered limits preserve existing high descriptors") {
    cinux::fs::FileTable table;
    fill_dynamic_descriptors(table);
    ASSERT_TRUE(table.capacity() >= 300);
    ASSERT_TRUE(!table.alloc(nullptr, nullptr, sample_node(1), {.limit = 64}).ok());
    ASSERT_TRUE(table.get(299).ok());
    ASSERT_TRUE(table.free(37).ok());
    ASSERT_EQ(table.alloc(nullptr, nullptr, sample_node(1), {.limit = 64}).value(), 37UL);
}

TEST("file table: sparse high descriptors survive clone and close-on-exec independently") {
    cinux::fs::FileTable parent;
    ASSERT_TRUE(parent.alloc(nullptr, nullptr, sample_node(7)).ok());
    ASSERT_TRUE(parent.duplicate_at(0, 8000).ok());
    ASSERT_TRUE(parent.set_cloexec(8000, true).ok());
    const auto kChild = parent.clone();
    ASSERT_TRUE(kChild.holds());
    kChild->close_on_exec();
    ASSERT_TRUE(!kChild->get(8000).ok());
    ASSERT_TRUE(parent.get(8000).ok());
    ASSERT_TRUE(kChild->get(0).ok());
}

int main() {
    return cinux::test::RunAll();
}
