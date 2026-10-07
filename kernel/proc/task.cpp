#include "kernel/proc/task.hpp"

#include "cinux/memory.hpp"
#include "cinux/ptr.hpp"
#include "kernel/arch/x86_64/context.hpp"
#include "kernel/proc/proc_config.hpp"

namespace cinux::proc {

namespace {

constexpr unsigned long long kX87ControlWord = 0x037FULL;
constexpr unsigned long long kMxcsrDefault   = 0x1F80ULL;

void plant_clean_fpu_image(cinux::arch::CpuContext& ctx) {
    cinux::base::SetBytes(ctx.fpu, 0, sizeof(ctx.fpu));
    auto* const kFcw   = reinterpret_cast<unsigned short*>(&ctx.fpu[0]);
    kFcw[0]            = static_cast<unsigned short>(kX87ControlWord);
    auto* const kMxcsr = reinterpret_cast<unsigned int*>(&ctx.fpu[4]);
    kMxcsr[0]          = static_cast<unsigned int>(kMxcsrDefault);
}

}  // namespace

void PrepareTaskContext(cinux::arch::CpuContext& ctx, const StartPlan& plan) {
    cinux::base::SetBytes(&ctx, 0, sizeof(ctx));
    plant_clean_fpu_image(ctx);
    auto* const kBottomWord = cinux::base::PtrAt<unsigned long long>(plan.stack_bottom);
    *kBottomWord            = kStackMagic;
    ctx.rsp                 = plan.stack_top;
    ctx.rip                 = plan.trampoline;
    ctx.r12                 = plan.entry;
    ctx.r13                 = plan.exit_hook;
}

}  // namespace cinux::proc
