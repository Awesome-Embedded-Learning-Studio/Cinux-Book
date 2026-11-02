#include "kernel/syscall/sys_prlimit.hpp"

#include "cinux/memory.hpp"
#include "kernel/fs/file_table.hpp"
#include "kernel/proc/proc_config.hpp"
#include "kernel/proc/task.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"
#include "kernel/syscall/uaccess.hpp"

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
    if (frame.rdi != 0 && frame.rdi != CurrentTask()->pid) {
        return -kEperm;
    }
    if (frame.rsi >= proc::kResourceLimitCount) {
        return -kEinval;
    }
    proc::Task* const        kSelf   = CurrentTask();
    const unsigned long long kOld[2] = {kSelf->limits[frame.rsi][0], kSelf->limits[frame.rsi][1]};
    if (frame.rdx != 0) {
        unsigned long long record[2] = {};
        if (!uaccess::CopyFromUser(record, frame.rdx, kLimitRecordBytes)) {
            return -kEfault;
        }
        if (record[0] > record[1]) {
            return -kEinval;
        }
        if (frame.rsi == 7 && record[1] > fs::kFileTableMax) {
            return -kEperm;
        }
        if (kSelf->euid != 0 && record[1] > kOld[1]) {
            return -kEperm;
        }
        kSelf->limits[frame.rsi][0] = record[0];
        kSelf->limits[frame.rsi][1] = record[1];
    }
    if (frame.r10 != 0 && !uaccess::CopyToUser(frame.r10, kOld, kLimitRecordBytes)) {
        return -kEfault;
    }
    return 0;
}

long long HandlePrctl(const SyscallFrame& frame) {
    proc::Task* const kSelf = CurrentTask();
    if (frame.rdi == 7) {  // PR_GET_KEEPCAPS
        return kSelf->keep_capabilities ? 1 : 0;
    }
    if (frame.rdi == 8) {  // PR_SET_KEEPCAPS, independently of the (empty) sets.
        if (frame.rsi > 1) { return -kEinval; }
        kSelf->keep_capabilities = frame.rsi != 0;
        return 0;
    }
    if (frame.rdi == kPrSetName) {
        if (frame.rsi == 0) {
            return -kEfault;
        }
        unsigned char incoming[kCommBytes] = {};
        if (!uaccess::CopyFromUser(incoming, frame.rsi, kCommBytes)) {
            return -kEfault;
        }
        base::SetBytes(kSelf->comm, 0, kCommBytes);
        unsigned long long index = 0;
        while (index < kCommNameBytes && incoming[index] != 0) {
            kSelf->comm[index] = static_cast<char>(incoming[index]);
            ++index;
        }
        return 0;
    }
    if (frame.rdi == kPrGetName) {
        if (frame.rsi == 0) {
            return -kEfault;
        }
        if (!uaccess::CopyToUser(frame.rsi, kSelf->comm, kCommBytes)) {
            return -kEfault;
        }
        return 0;
    }
    return -kEinval;
}

}  // namespace cinux::syscall
