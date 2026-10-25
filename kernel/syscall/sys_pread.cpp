#include "kernel/syscall/sys_pread.hpp"

#include "cinux/result.hpp"
#include "cinux/shared.hpp"
#include "kernel/fs/file_table.hpp"
#include "kernel/fs/open_file.hpp"
#include "kernel/proc/task.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"
#include "kernel/syscall/uaccess.hpp"

namespace cinux::syscall {

namespace {

constexpr unsigned long long kPreadStageBytes = 1024;

}  // namespace

long long HandlePread(const SyscallFrame& frame) {
    if (frame.rdi >= fs::kFileTableMax) {
        return -kEbadf;
    }
    if (frame.rsi >= kUserAddressLimit) {
        return -kEfault;
    }
    const auto kOffset = static_cast<long long>(frame.r10);
    if (kOffset < 0) {
        return -kEinval;
    }
    const base::Result<base::Shared<fs::OpenFile>> kSlot = CurrentTask()->files->shared(frame.rdi);
    if (!kSlot.ok()) {
        return -kEbadf;
    }
    const auto* const kFile = kSlot.value().get();
    unsigned char     stage[kPreadStageBytes];
    const auto        kWant =
        static_cast<unsigned long>(frame.rdx < kPreadStageBytes ? frame.rdx : kPreadStageBytes);
    const base::Result<unsigned long> kGot = kFile->ops->read(
        kFile->backend, kFile->node, static_cast<unsigned long>(kOffset), stage, kWant);
    if (!kGot.ok()) {
        return -ErrnoFrom(kGot.error());
    }
    if (!uaccess::CopyToUser(frame.rsi, stage, kGot.value())) {
        return -kEfault;
    }
    return static_cast<long long>(kGot.value());
}

}  // namespace cinux::syscall
