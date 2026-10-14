#include "kernel/arch/x86_64/usermode.hpp"

#include "cinux/bit_ops/bitmask.hpp"
#include "kernel/arch/x86_64/gdt.hpp"
#include "kernel/arch/x86_64/instructions.hpp"
#include "kernel/arch/x86_64/msr.hpp"
#include "kernel/syscall/syscall.hpp"

namespace cinux::arch::usermode {

namespace {

constexpr unsigned long long kInterruptFlagMask =
    cinux::base::bit::MaskBit<unsigned long long>(9).raw;

constexpr unsigned long long kStarValue =
    (static_cast<unsigned long long>(gdt::kSysretStarBase) << 48U) |
    (static_cast<unsigned long long>(gdt::kSelectorCode) << 32U);

static_assert((kStarValue >> 48U) == gdt::kSysretStarBase, "SYSRET base keeps its RPL-3 folding");
static_assert(((kStarValue >> 32U) & 0xFFFFU) == gdt::kSelectorCode,
              "SYSCALL entries start on the kernel code selector");

}  // namespace

void EnableFastSystemCalls() {
    unsigned long long const kEfer = ReadMsr(msr::kEfer) | msr::kEferSyscallEnable;
    WriteMsr(msr::kEfer, kEfer);
    WriteMsr(msr::kStar, kStarValue);
    WriteMsr(msr::kLstar, reinterpret_cast<unsigned long long>(&SyscallEntry));
    WriteMsr(msr::kSfmask, kInterruptFlagMask);
}

}  // namespace cinux::arch::usermode
