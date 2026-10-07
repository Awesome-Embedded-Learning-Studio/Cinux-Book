#include <cstddef>

#include "../framework/framework.hpp"
#include "kernel/arch/x86_64/context.hpp"
#include "kernel/proc/proc_config.hpp"
#include "kernel/proc/task.hpp"
#include "test_assert.hpp"

namespace {

constexpr unsigned long long kFakeTrampoline = 0x1111ULL;
constexpr unsigned long long kFakeEntry      = 0x2222ULL;
constexpr unsigned long long kFakeExitHook   = 0x3333ULL;

struct FakeStack {
    unsigned long long slots[64];
};

cinux::proc::StartPlan plan_for(FakeStack& stack) {
    return cinux::proc::StartPlan{
        .trampoline   = kFakeTrampoline,
        .entry        = kFakeEntry,
        .exit_hook    = kFakeExitHook,
        .stack_bottom = reinterpret_cast<unsigned long long>(&stack.slots[0]),
        .stack_top    = reinterpret_cast<unsigned long long>(&stack.slots[64])};
}

TEST("context: layout matches the assembly contract") {
    using cinux::arch::CpuContext;
    static_assert(offsetof(CpuContext, fpu) == 64);
    static_assert(sizeof(CpuContext) == 576);
    ASSERT_TRUE(alignof(CpuContext) >= 16);
}

TEST("context: arming a fresh image points at the trampoline pair") {
    cinux::arch::CpuContext ctx{};
    FakeStack               stack{};
    cinux::proc::PrepareTaskContext(ctx, plan_for(stack));
    ASSERT_TRUE(ctx.rip == kFakeTrampoline);
    ASSERT_TRUE(ctx.r12 == kFakeEntry);
    ASSERT_TRUE(ctx.r13 == kFakeExitHook);
    ASSERT_TRUE(ctx.rsp == reinterpret_cast<unsigned long long>(&stack.slots[64]));
    ASSERT_TRUE(ctx.r15 == 0);
}

TEST("context: overflow magic lands at the stack bottom word") {
    cinux::arch::CpuContext ctx{};
    FakeStack               stack{};
    cinux::proc::PrepareTaskContext(ctx, plan_for(stack));
    ASSERT_TRUE(stack.slots[0] == cinux::proc::kStackMagic);
    ASSERT_TRUE(stack.slots[1] == 0);
}

TEST("context: clean fpu image carries default control words") {
    cinux::arch::CpuContext ctx{};
    FakeStack               stack{};
    cinux::proc::PrepareTaskContext(ctx, plan_for(stack));
    const auto* const kImage = reinterpret_cast<const unsigned short*>(&ctx.fpu[0]);
    ASSERT_TRUE(kImage[0] == 0x037F);
    const auto* const kMxcsr = reinterpret_cast<const unsigned int*>(&ctx.fpu[4]);
    ASSERT_TRUE(kMxcsr[0] == 0x1F80);
    ASSERT_TRUE(ctx.fpu[1] == 0);
    ASSERT_TRUE(ctx.fpu[63] == 0);
}

}  // namespace

int main() {
    return cinux::test::RunAll();
}
