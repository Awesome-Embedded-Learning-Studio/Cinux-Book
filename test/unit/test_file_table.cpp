#include "../framework/framework.hpp"
#include "cinux/result.hpp"
#include "kernel/fs/file_table.hpp"
#include "kernel/fs/vfs.hpp"
#include "test/mock/file_refs.hpp"
#include "test_assert.hpp"

namespace {

cinux::fs::FsNode sample_node(unsigned long ino) {
    return cinux::fs::FsNode{
        .opaque = nullptr, .type = cinux::fs::InodeType::kFile, .ino = ino, .size = 0};
}

}  // namespace

TEST("file table: allocation starts at the first descriptor past the streams") {
    cinux::fs::FileTable table;
    const auto           kFirst = table.alloc(nullptr, nullptr, sample_node(1));
    ASSERT_TRUE(kFirst.ok());
    ASSERT_EQ(kFirst.value(), cinux::fs::kFirstDescriptor);
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
    for (unsigned long fd = cinux::fs::kFirstDescriptor; fd < cinux::fs::kFileTableMax; ++fd) {
        ASSERT_TRUE(table.alloc(nullptr, nullptr, sample_node(fd)).ok());
    }
    const auto kOverflow = table.alloc(nullptr, nullptr, sample_node(99));
    ASSERT_FALSE(kOverflow.ok());
    ASSERT_EQ(kOverflow.error(), cinux::base::KernelError::kOutOfMemory);
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
    const auto kSlot = table.get(cinux::fs::kFirstDescriptor);
    ASSERT_TRUE(kSlot.ok());
    ASSERT_EQ(kSlot.value()->node.ino, 7UL);
    ASSERT_EQ(kSlot.value()->offset, 0UL);
    kSlot.value()->offset += 12;
    const auto kReread = table.get(cinux::fs::kFirstDescriptor);
    ASSERT_EQ(kReread.value()->offset, 12UL);
    ASSERT_TRUE(table.free(cinux::fs::kFirstDescriptor).ok());
    const auto kAfter = table.get(cinux::fs::kFirstDescriptor);
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

TEST("file table: allocation failure does not retain a node") {
    cinux::test::FileRefs refs;
    cinux::fs::FileTable  table;
    for (unsigned long fd = cinux::fs::kFirstDescriptor; fd < cinux::fs::kFileTableMax; ++fd) {
        ASSERT_TRUE(table.alloc(&cinux::test::FileRefs::kOps, &refs, sample_node(fd)).ok());
    }
    const auto kBefore = refs.retained;
    ASSERT_FALSE(table.alloc(&cinux::test::FileRefs::kOps, &refs, sample_node(99)).ok());
    ASSERT_EQ(refs.retained, kBefore);
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

int main() {
    return cinux::test::RunAll();
}
