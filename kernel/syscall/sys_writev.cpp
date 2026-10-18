#include "kernel/syscall/sys_writev.hpp"

#include "kernel/syscall/sys_write.hpp"
#include "kernel/syscall/syscall.hpp"

namespace cinux::syscall {

namespace {

/// One scatter segment as the hosted caller lays it out: a base and a
/// length, packed with no padding on x86-64.
struct IoVector {
    unsigned long long base;
    unsigned long long bytes;
};

}  // namespace

long long HandleWritev(const SyscallFrame& frame) {
    const unsigned long long kSegments = frame.rdx;
    if (kSegments > 1024) {
        return -kEinval;
    }
    if (kSegments != 0 && frame.rsi >= kUserAddressLimit) {
        return -kEfault;
    }
    auto const* const kRuns =
        reinterpret_cast<const IoVector*>(frame.rsi);  // NOLINT(performance-no-int-to-ptr)
    long long total = 0;
    for (unsigned long long index = 0; index < kSegments; ++index) {
        const long long kPut = WriteRun(frame.rdi, kRuns[index].base, kRuns[index].bytes);
        if (kPut < 0) {
            return total > 0 ? total : kPut;
        }
        total += kPut;
    }
    return total;
}

}  // namespace cinux::syscall
