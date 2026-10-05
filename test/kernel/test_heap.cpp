#include "cinux/literal_types.hpp"
#include "framework_kernel.hpp"
#include "kernel/boot/print.hpp"
#include "kernel/mm/heap.hpp"
#include "test_assert.hpp"
#include "test_case.hpp"  // NOLINT(misc-include-cleaner) consumed by the TEST() macro body

namespace {

using cinux::base::operator""_KiB;

TEST("heap: new and delete round trip through operator new") {
    auto* const kBlock = new unsigned long[64];
    kBlock[0]          = 0x114514UL;
    kBlock[63]         = 0x200330UL;
    auto const kHead   = kBlock[0];
    auto const kTail   = kBlock[63];
    delete[] kBlock;
    ASSERT_TRUE(kHead == 0x114514UL);
    ASSERT_TRUE(kTail == 0x200330UL);
}

TEST("heap: a large allocation grows the span and stays writable") {
    unsigned long const kTopBefore = cinux::mm::Heap::self().top();
    auto* const         kBig       = new unsigned char[100_KiB];
    unsigned long const kTopAfter  = cinux::mm::Heap::self().top();
    kBig[0]                        = 1;
    kBig[100_KiB - 1]              = 2;
    int const  kHead               = kBig[0];
    int const  kTail               = kBig[100_KiB - 1];
    auto const kSpot               = reinterpret_cast<unsigned long>(kBig);
    cinux::print::Println("[ktest] heap grew: top %X -> %X, block %X", kTopBefore, kTopAfter,
                          kSpot);
    delete[] kBig;
    ASSERT_TRUE(kTopAfter > kTopBefore);
    ASSERT_EQ(kHead, 1);
    ASSERT_EQ(kTail, 2);
}

TEST("heap: freed blocks are reused before the span grows again") {
    auto* const         kFirst  = new unsigned long(1);
    auto* const         kSecond = new unsigned long(2);
    unsigned long const kTop    = cinux::mm::Heap::self().top();
    delete kFirst;
    auto* const kThird     = new unsigned long(3);
    auto const  kFirstSpot = reinterpret_cast<unsigned long>(kFirst);
    auto const  kThirdSpot = reinterpret_cast<unsigned long>(kThird);
    cinux::print::Println("[ktest] heap reuse: first %X third %X", kFirstSpot, kThirdSpot);
    delete kSecond;
    delete kThird;
    ASSERT_TRUE(kThirdSpot == kFirstSpot);
    ASSERT_TRUE(cinux::mm::Heap::self().top() == kTop);
}

}  // namespace
