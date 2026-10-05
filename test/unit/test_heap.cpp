#include "../framework/framework.hpp"
#include "cinux/literal_types.hpp"
#include "kernel/mm/heap.hpp"
#include "test_assert.hpp"

namespace {

using cinux::base::operator""_GiB;
using cinux::base::operator""_KiB;

alignas(16) unsigned char g_pool[64 * 1024];

unsigned long pool_base() {
    return reinterpret_cast<unsigned long>(g_pool);
}

}  // namespace

TEST("heap: init claims the span as one free block") {
    cinux::mm::Heap heap;
    ASSERT_TRUE(heap.init(pool_base(), 16_KiB));
    ASSERT_TRUE(heap.free_bytes() == 16_KiB - 32 - 8);
    ASSERT_TRUE(heap.top() == pool_base() + 16_KiB);
    ASSERT_TRUE(!heap.init(pool_base() + 8, 16_KiB));
}

TEST("heap: allocate returns aligned payloads and spends free bytes") {
    cinux::mm::Heap heap;
    ASSERT_TRUE(heap.init(pool_base(), 16_KiB));
    unsigned long const kBefore = heap.free_bytes();
    // NOLINTNEXTLINE(misc-const-correctness)
    void* const         kBlock  = heap.allocate(64);
    ASSERT_TRUE(kBlock != nullptr);
    ASSERT_TRUE(reinterpret_cast<unsigned long>(kBlock) % 16 == 0);
    ASSERT_TRUE(heap.free_bytes() == kBefore - 64 - 32 - 8);
    ASSERT_TRUE(heap.allocate(1) != nullptr);
}

TEST("heap: free restores the span through coalescing") {
    cinux::mm::Heap heap;
    ASSERT_TRUE(heap.init(pool_base(), 16_KiB));
    unsigned long const kBefore = heap.free_bytes();
    void* const         kFirst  = heap.allocate(64);
    void* const         kSecond = heap.allocate(64);
    void* const         kThird  = heap.allocate(64);
    ASSERT_TRUE(heap.free(kSecond));
    ASSERT_TRUE(heap.free(kFirst));
    ASSERT_TRUE(heap.free(kThird));
    ASSERT_TRUE(heap.free_bytes() == kBefore);
    ASSERT_TRUE(heap.free(kThird) == false);
}

TEST("heap: first fit reuses the lowest fitting hole") {
    cinux::mm::Heap heap;
    ASSERT_TRUE(heap.init(pool_base(), 16_KiB));
    void* const kFirst  = heap.allocate(64);
    // NOLINTNEXTLINE(misc-const-correctness)
    void* const kSecond = heap.allocate(64);
    ASSERT_TRUE(heap.free(kFirst));
    // NOLINTNEXTLINE(misc-const-correctness)
    void* const kAgain = heap.allocate(32);
    ASSERT_TRUE(kAgain == kFirst);
    ASSERT_TRUE(heap.allocate(16) != nullptr);
    ASSERT_TRUE(kSecond != kFirst);
}

TEST("heap: refused pointers stay refused") {
    cinux::mm::Heap heap;
    ASSERT_TRUE(heap.init(pool_base(), 16_KiB));
    ASSERT_TRUE(!heap.free(nullptr));
    ASSERT_TRUE(!heap.free(g_pool + 7));
    ASSERT_TRUE(!heap.free(g_pool + 64_KiB + 32));
    ASSERT_TRUE(heap.allocate(1_GiB) == nullptr);
}

TEST("heap: grow appends usable space") {
    cinux::mm::Heap heap;
    ASSERT_TRUE(heap.init(pool_base(), 16_KiB));
    // NOLINTNEXTLINE(misc-const-correctness)
    void* const kFiller = heap.allocate(12_KiB);
    ASSERT_TRUE(kFiller != nullptr);
    ASSERT_TRUE(heap.allocate(8_KiB) == nullptr);
    ASSERT_TRUE(heap.grow(pool_base() + 32_KiB));
    // NOLINTNEXTLINE(misc-const-correctness)
    void* const kLate = heap.allocate(8_KiB);
    ASSERT_TRUE(kLate != nullptr);
    ASSERT_TRUE(heap.top() == pool_base() + 32_KiB);
    ASSERT_TRUE(heap.grow(pool_base() + 32_KiB) == false);
}

int main() {
    return cinux::test::RunAll();
}
