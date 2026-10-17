#include "kernel/syscall/sys_arch_prctl.hpp"

#include "kernel/arch/x86_64/context.hpp"
#include "kernel/arch/x86_64/per_cpu.hpp"
#include "kernel/proc/task.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"

namespace cinux::syscall {

namespace {

/// arch_prctl op: make addr the caller's GS base.
inline constexpr Word kArchSetGs = 0x1001;
/// arch_prctl op: make addr the caller's FS base.
inline constexpr Word kArchSetFs = 0x1002;
/// arch_prctl op: report the caller's FS base through addr.
inline constexpr Word kArchGetFs = 0x1003;
/// arch_prctl op: report the caller's GS base through addr.
inline constexpr Word kArchGetGs = 0x1004;

}  // namespace

long long HandleArchPrctl(const SyscallFrame& frame) {
    proc::Task* const kSelf = CurrentTask();
    switch (frame.rdi) {
    case kArchSetFs:
        if (!cinux::arch::IsCanonicalAddress(frame.rsi)) {
            return -kEinval;
        }
        kSelf->ctx.fs_base = frame.rsi;
        cinux::arch::per_cpu::WriteFsBase(frame.rsi);
        return 0;
    case kArchSetGs:
        if (!cinux::arch::IsCanonicalAddress(frame.rsi)) {
            return -kEinval;
        }
        kSelf->ctx.gs_base = frame.rsi;
        cinux::arch::per_cpu::WriteKernelGsBase(frame.rsi);
        return 0;
    case kArchGetFs: {
        if (!UserRangeWritable(frame.rsi, sizeof(Word))) {
            return -kEfault;
        }
        auto* const kOut = reinterpret_cast<Word*>(frame.rsi);  // NOLINT(performance-no-int-to-ptr)
        *kOut            = kSelf->ctx.fs_base;
        return 0;
    }
    case kArchGetGs: {
        if (!UserRangeWritable(frame.rsi, sizeof(Word))) {
            return -kEfault;
        }
        auto* const kOut = reinterpret_cast<Word*>(frame.rsi);  // NOLINT(performance-no-int-to-ptr)
        *kOut            = kSelf->ctx.gs_base;
        return 0;
    }
    default:
        return -kEinval;
    }
}

}  // namespace cinux::syscall
