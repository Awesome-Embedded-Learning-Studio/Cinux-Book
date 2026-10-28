#include "kernel/syscall/sys_writev.hpp"

#include <utility>

#include "kernel/syscall/sys_write.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/uaccess.hpp"

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
    long long total = 0;
    for (unsigned long long index = 0; index < kSegments; ++index) {
        IoVector run{};
        if (!uaccess::CopyFromUser(&run, frame.rsi + (index * sizeof(run)), sizeof(run))) {
            return total > 0 ? total : -kEfault;
        }
        const long long kPut = WriteRun(frame.rdi, run.base, run.bytes);
        if (kPut < 0) {
            return total > 0 ? total : kPut;
        }
        total += kPut;
        if (std::cmp_less(kPut, run.bytes)) {
            return total;
        }
    }
    return total;
}

}  // namespace cinux::syscall
