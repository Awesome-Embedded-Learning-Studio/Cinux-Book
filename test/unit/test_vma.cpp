#include "../framework/framework.hpp"
#include "cinux/result.hpp"
#include "kernel/mm/vma.hpp"
#include "test_assert.hpp"

namespace {

cinux::mm::VmaRegion range_at(unsigned long start, unsigned long end) {
    cinux::mm::VmaRegion range{};
    range.start    = start;
    range.end      = end;
    range.kind     = cinux::mm::VmaKind::kAnonymous;
    range.readable = true;
    range.writable = true;
    return range;
}

TEST("vma: registered ranges answer find inside, at start, near end") {
    cinux::mm::VmaStore store;
    ASSERT_TRUE(store.register_range(range_at(0x1000, 0x3000)).ok());
    ASSERT_TRUE(store.register_range(range_at(0x5000, 0x6000)).ok());

    ASSERT_TRUE(store.find(0x1000) != nullptr);
    ASSERT_TRUE(store.find(0x2FFF) != nullptr);
    ASSERT_TRUE(store.find(0x5800) != nullptr);
    ASSERT_EQ(store.find(0x1000)->end, 0x3000UL);
}

TEST("vma: addresses outside every range find nothing") {
    cinux::mm::VmaStore store;
    ASSERT_TRUE(store.register_range(range_at(0x1000, 0x3000)).ok());
    ASSERT_TRUE(store.register_range(range_at(0x5000, 0x6000)).ok());

    ASSERT_TRUE(store.find(0x0FFF) == nullptr);
    ASSERT_TRUE(store.find(0x3000) == nullptr);
    ASSERT_TRUE(store.find(0x4FFF) == nullptr);
    ASSERT_TRUE(store.find(0x6000) == nullptr);
}

TEST("vma: empty and inverted ranges are refused") {
    cinux::mm::VmaStore store;
    ASSERT_TRUE(!store.register_range(range_at(0x2000, 0x2000)).ok());
    ASSERT_TRUE(!store.register_range(range_at(0x3000, 0x2000)).ok());
    ASSERT_TRUE(store.empty());
}

TEST("vma: overlaps inside, across and duplicate are refused") {
    cinux::mm::VmaStore store;
    ASSERT_TRUE(store.register_range(range_at(0x1000, 0x3000)).ok());

    ASSERT_TRUE(!store.register_range(range_at(0x2000, 0x4000)).ok());
    ASSERT_TRUE(!store.register_range(range_at(0x0000, 0x1001)).ok());
    ASSERT_TRUE(!store.register_range(range_at(0x1000, 0x3000)).ok());
    ASSERT_TRUE(!store.register_range(range_at(0x1800, 0x2800)).ok());
}

TEST("vma: touching ranges file side by side") {
    cinux::mm::VmaStore store;
    ASSERT_TRUE(store.register_range(range_at(0x1000, 0x3000)).ok());
    ASSERT_TRUE(store.register_range(range_at(0x3000, 0x4000)).ok());
    ASSERT_TRUE(store.find(0x3500)->start == 0x3000UL);
}

TEST("vma: clone carries every range over") {
    cinux::mm::VmaStore store;
    ASSERT_TRUE(store.register_range(range_at(0x1000, 0x2000)).ok());
    ASSERT_TRUE(store.register_range(range_at(0x4000, 0x5000)).ok());

    const cinux::mm::VmaStore* const kChild = store.clone();
    ASSERT_TRUE(kChild != nullptr);
    ASSERT_TRUE(kChild->find(0x1800) != nullptr);
    ASSERT_TRUE(kChild->find(0x4500) != nullptr);
    delete kChild;
}

TEST("vma: child teardown leaves the parent register intact") {
    cinux::mm::VmaStore store;
    ASSERT_TRUE(store.register_range(range_at(0x1000, 0x2000)).ok());

    cinux::mm::VmaStore* const kChild = store.clone();
    ASSERT_TRUE(kChild != nullptr);
    kChild->teardown();
    ASSERT_TRUE(kChild->empty());
    delete kChild;
    ASSERT_TRUE(store.find(0x1800) != nullptr);
}

TEST("vma: cloning an empty register yields an empty register") {
    const cinux::mm::VmaStore        kEmptyStore;
    const cinux::mm::VmaStore* const kEmptyChild = kEmptyStore.clone();
    ASSERT_TRUE(kEmptyChild != nullptr);
    ASSERT_TRUE(kEmptyChild->empty());
    delete kEmptyChild;
}

TEST("vma: teardown empties the register for reuse") {
    cinux::mm::VmaStore store;
    ASSERT_TRUE(store.register_range(range_at(0x1000, 0x2000)).ok());
    store.teardown();
    ASSERT_TRUE(store.empty());
    ASSERT_TRUE(store.find(0x1800) == nullptr);
}

TEST("vma: the spot hunt starts at the ceiling and drops below blockers") {
    cinux::mm::VmaStore                      store;
    const cinux::base::Result<unsigned long> kFromCeiling = store.find_free_spot(0x10000, 0x2000);
    ASSERT_EQ(kFromCeiling.value(), 0xE000UL);
    ASSERT_TRUE(store.register_range(range_at(0xB000, 0xF000)).ok());
    const cinux::base::Result<unsigned long> kHunted = store.find_free_spot(0x10000, 0x2000);
    ASSERT_EQ(kHunted.value(), 0x9000UL);
}

TEST("vma: a stretch wider than any gap finds nothing") {
    cinux::mm::VmaStore store;
    ASSERT_TRUE(store.register_range(range_at(0x0000, 0xF000)).ok());
    ASSERT_TRUE(!store.find_free_spot(0x10000, 0x2000).ok());
    ASSERT_TRUE(store.find_free_spot(0x10000, 0x1000).ok());
}

TEST("vma: the seeded heap answers find and moves by brk") {
    cinux::mm::VmaStore store;
    store.seed_heap(0x100000, 0x200000);
    ASSERT_TRUE(store.find(0x100000) == nullptr);
    const cinux::base::Result<unsigned long> kQueried = store.move_heap_end(0);
    ASSERT_EQ(kQueried.value(), 0x100000UL);
    const cinux::base::Result<unsigned long> kMoved = store.move_heap_end(0x101234);
    ASSERT_EQ(kMoved.value(), 0x102000UL);
    ASSERT_TRUE(store.find(0x101FFF) != nullptr);
    ASSERT_TRUE(store.find(0x102000) == nullptr);
}

TEST("vma: the break clamps to its floor and ceiling") {
    cinux::mm::VmaStore store;
    store.seed_heap(0x100000, 0x180000);
    const cinux::base::Result<unsigned long> kCapped = store.move_heap_end(0x400000);
    ASSERT_EQ(kCapped.value(), 0x180000UL);
    const cinux::base::Result<unsigned long> kFloored = store.move_heap_end(0x100);
    ASSERT_EQ(kFloored.value(), 0x100000UL);
    ASSERT_EQ(store.heap_end(), 0x100000UL);
}

TEST("vma: brk on a world without a heap refuses") {
    cinux::mm::VmaStore store;
    ASSERT_TRUE(!store.move_heap_end(0x1000).ok());
    ASSERT_EQ(store.heap_end(), 0UL);
}

TEST("vma: unregister withdraws whole spans inside the stretch") {
    cinux::mm::VmaStore store;
    ASSERT_TRUE(store.register_range(range_at(0x1000, 0x2000)).ok());
    ASSERT_TRUE(store.register_range(range_at(0x3000, 0x4000)).ok());
    ASSERT_TRUE(store.unregister_range(0x1000, 0x4000).ok());
    ASSERT_TRUE(store.empty());
    ASSERT_TRUE(store.find(0x1800) == nullptr);
}

TEST("vma: unregister refuses to split a span") {
    cinux::mm::VmaStore store;
    ASSERT_TRUE(store.register_range(range_at(0x1000, 0x3000)).ok());
    ASSERT_TRUE(!store.unregister_range(0x1000, 0x2000).ok());
    ASSERT_TRUE(store.find(0x2500) != nullptr);
}

TEST("vma: unregister empties a wholly withdrawn heap") {
    cinux::mm::VmaStore store;
    store.seed_heap(0x100000, 0x200000);
    ASSERT_TRUE(store.move_heap_end(0x102000).ok());
    ASSERT_TRUE(store.unregister_range(0x100000, 0x103000).ok());
    ASSERT_EQ(store.heap_end(), 0x100000UL);
    ASSERT_TRUE(store.find(0x101000) == nullptr);
}

TEST("vma: protect rewrites the terms of spans it wholly owns") {
    cinux::mm::VmaStore store;
    ASSERT_TRUE(store.register_range(range_at(0x1000, 0x2000)).ok());
    const cinux::base::Result<void> kNewTerms =
        store.protect(0x1000, 0x2000, {.readable = true, .writable = false});
    ASSERT_TRUE(kNewTerms.ok());
    ASSERT_TRUE(store.find(0x1800)->readable);
    ASSERT_TRUE(!store.find(0x1800)->writable);
}

TEST("vma: protect refuses to split a span") {
    cinux::mm::VmaStore store;
    ASSERT_TRUE(store.register_range(range_at(0x1000, 0x3000)).ok());
    const cinux::base::Result<void> kSplit =
        store.protect(0x1000, 0x2000, {.readable = true, .writable = false});
    ASSERT_TRUE(!kSplit.ok());
    ASSERT_TRUE(store.find(0x1800)->writable);
}

TEST("vma: max_end reports the highest recorded end") {
    cinux::mm::VmaStore store;
    ASSERT_TRUE(store.register_range(range_at(0x1000, 0x2000)).ok());
    ASSERT_TRUE(store.register_range(range_at(0x5000, 0x9000)).ok());
    ASSERT_EQ(store.max_end(), 0x9000UL);
    store.seed_heap(0x100000, 0x200000);
    ASSERT_TRUE(store.move_heap_end(0x110000).ok());
    ASSERT_EQ(store.max_end(), 0x110000UL);
}

}  // namespace

int main() {
    return cinux::test::RunAll();
}
