#include "kernel/syscall/sys_prlimit.hpp"

#include "cinux/memory.hpp"
#include "kernel/proc/proc_config.hpp"
#include "kernel/proc/task.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"

namespace cinux::syscall {

namespace {

/// The record prlimit64 copies in and out: one limits row of the task
/// itself, so the width cannot drift from the table it lands in.
constexpr unsigned long long kLimitRecordBytes = sizeof(proc::Task::limits[0]);

/// prctl op: name the calling task, at most fifteen name bytes.
constexpr unsigned long long kPrSetName = 15;
/// prctl op: read the calling task's full sixteen-byte name back.
constexpr unsigned long long kPrGetName = 16;

/// The comm record's whole width and its name budget, the trailing NUL
/// included; both derive from the task's own array, so the uapi number
/// lives only in that array's width.
constexpr unsigned long long kCommBytes     = sizeof(proc::Task::comm);
constexpr unsigned long long kCommNameBytes = sizeof(proc::Task::comm) - 1;

}  // namespace

long long HandlePrlimit64(const SyscallFrame& frame) {
    if (frame.rdi != 0) {
        return -kEperm;
    }
    if (frame.rsi >= proc::kResourceLimitCount) {
        return -kEinval;
    }
    proc::Task* const kSelf = CurrentTask();
    if (frame.rdx != 0) {
        if (frame.rdx >= kUserAddressLimit || !UserRangeReadable(frame.rdx, kLimitRecordBytes)) {
            return -kEfault;
        }
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        const auto* const kRecord = reinterpret_cast<const unsigned long long*>(frame.rdx);
        if (kRecord[0] > kRecord[1] && kRecord[1] != proc::kResourceLimitInfinity) {
            return -kEperm;
        }
        kSelf->limits[frame.rsi][0] = kRecord[0];
        kSelf->limits[frame.rsi][1] = kRecord[1];
    }
    if (frame.r10 != 0) {
        if (frame.r10 >= kUserAddressLimit || !UserRangeWritable(frame.r10, kLimitRecordBytes)) {
            return -kEfault;
        }
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        auto* const kOld = reinterpret_cast<unsigned long long*>(frame.r10);
        kOld[0]          = kSelf->limits[frame.rsi][0];
        kOld[1]          = kSelf->limits[frame.rsi][1];
    }
    return 0;
}

long long HandlePrctl(const SyscallFrame& frame) {
    proc::Task* const kSelf = CurrentTask();
    if (frame.rdi == kPrSetName) {
        if (frame.rsi == 0 || !UserRangeReadable(frame.rsi, kCommBytes)) {
            return -kEfault;
        }
        base::SetBytes(kSelf->comm, 0, kCommBytes);
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        const auto* const  kFrom = reinterpret_cast<const unsigned char*>(frame.rsi);
        unsigned long long index = 0;
        while (index < kCommNameBytes && kFrom[index] != 0) {
            kSelf->comm[index] = static_cast<char>(kFrom[index]);
            ++index;
        }
        return 0;
    }
    if (frame.rdi == kPrGetName) {
        if (frame.rsi == 0 || !UserRangeWritable(frame.rsi, kCommBytes)) {
            return -kEfault;
        }
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        base::CopyBytes(reinterpret_cast<void*>(frame.rsi), kSelf->comm, kCommBytes);
        return 0;
    }
    return -kEinval;
}

}  // namespace cinux::syscall
