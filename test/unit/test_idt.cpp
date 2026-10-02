#include "../framework/framework.hpp"
#include "kernel/arch/x86_64/idt.hpp"

using cinux::arch::idt::EncodeGate;
using cinux::arch::idt::GateEntry;
using cinux::arch::idt::kTypeInterruptGate;
using cinux::arch::idt::kVectorCount;

TEST("idt: gate entry is sixteen bytes on LP64") {
    ASSERT_TRUE(sizeof(GateEntry) == 16);
    ASSERT_TRUE(sizeof(cinux::arch::idt::TablePointer) == 10);
    ASSERT_TRUE(kVectorCount * sizeof(GateEntry) == 4096);
}

TEST("idt: handler address splits across three offset fields") {
    auto const kGate = EncodeGate(0x1122334455667788ULL, 0x08, 0, kTypeInterruptGate);
    ASSERT_TRUE(kGate.offset_low == 0x7788);
    ASSERT_TRUE(kGate.offset_mid == 0x5566);
    ASSERT_TRUE(kGate.offset_high == 0x11223344);
    ASSERT_TRUE(kGate.reserved == 0);
}

TEST("idt: gate carries selector, ist and interrupt-gate attributes") {
    auto const kGate = EncodeGate(0x1000, 0x08, 0, kTypeInterruptGate);
    ASSERT_TRUE(kGate.selector == 0x08);
    ASSERT_TRUE(kGate.ist == 0);
    ASSERT_TRUE(kGate.type_attr == 0x8E);
}

TEST("idt: interrupt gate bit pattern is present+dpl0+type-e") {
    auto const kGate = EncodeGate(0, 0, 0, kTypeInterruptGate);
    ASSERT_TRUE(((kGate.type_attr >> 7) & 1U) == 1U);
    ASSERT_TRUE(((kGate.type_attr >> 5) & 3U) == 0U);
    ASSERT_TRUE((kGate.type_attr & 0xFU) == 0xEU);
}

TEST("idt: high kernel address encodes without losing bits") {
    auto const kHandler = 0xFFFFFFFF80200000ULL;
    auto const kGate    = EncodeGate(kHandler, 0x08, 0, kTypeInterruptGate);
    auto const kJoined  = (static_cast<unsigned long long>(kGate.offset_high) << 32) |
                          (static_cast<unsigned long long>(kGate.offset_mid) << 16) |
                          kGate.offset_low;
    ASSERT_TRUE(kJoined == kHandler);
}

int main() {
    return cinux::test::RunAll();
}
