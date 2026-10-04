#include "framework_kernel.hpp"
#include "kernel/arch/x86_64/gdt.hpp"
#include "kernel/arch/x86_64/idt.hpp"
#include "kernel/arch/x86_64/isr.hpp"
#include "test_assert.hpp"
#include "test_case.hpp"  // NOLINT(misc-include-cleaner) consumed by the TEST() macro body

namespace {

constexpr unsigned long long kVectorInvalidOpcode = 6;

constexpr unsigned long long kUd2Length = 2;

constexpr unsigned long long kArmedGates = 256;

TEST("gdt: the machine runs on the kernel's own table") {
    // NOLINTBEGIN(misc-const-correctness)
    cinux::arch::gdt::TablePointer gdtr{};
    // NOLINTEND(misc-const-correctness)
    asm volatile("sgdt %0" : "=m"(gdtr) : : "memory");
    ASSERT_EQ(gdtr.limit, sizeof(cinux::arch::gdt::KernelGdt) - 1);

    // NOLINTBEGIN(misc-const-correctness)
    unsigned short code_segment = 0;
    // NOLINTEND(misc-const-correctness)
    asm volatile("movw %%cs, %0" : "=r"(code_segment));
    ASSERT_EQ(code_segment, cinux::arch::gdt::kSelectorCode);
}

TEST("idt: the table stands armed with 256 gates") {
    // NOLINTBEGIN(misc-const-correctness)
    cinux::arch::idt::TablePointer idtr{};
    // NOLINTEND(misc-const-correctness)
    asm volatile("sidt %0" : "=m"(idtr) : : "memory");
    ASSERT_EQ(idtr.limit, (kArmedGates * sizeof(cinux::arch::idt::GateEntry)) - 1);
}

TEST("exception: an armed ud2 hurts, heals, and runs on") {
    ASSERT_TRUE(cinux::arch::isr::ArmRecoverableFault(kVectorInvalidOpcode, kUd2Length));
    asm volatile("ud2");
    ASSERT_EQ(cinux::arch::isr::TakeRecoveredVector(), kVectorInvalidOpcode);
}

}  // namespace
