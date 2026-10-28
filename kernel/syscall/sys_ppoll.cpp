#include "kernel/syscall/sys_ppoll.hpp"

#include "cinux/memory.hpp"
#include "cinux/result.hpp"
#include "cinux/shared.hpp"
#include "kernel/fs/open_file.hpp"
#include "kernel/fs/pipe.hpp"
#include "kernel/net/socket.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/signals.hpp"
#include "kernel/proc/task.hpp"
#include "kernel/syscall/signal.hpp"
#include "kernel/syscall/sys_clock.hpp"
#include "kernel/syscall/sys_eventfd.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"
#include "kernel/syscall/uaccess.hpp"
#include "kernel/syscall/wait_time.hpp"

namespace cinux::syscall {

namespace {

constexpr unsigned int kPollNval = 0x0020U;

constexpr unsigned long kPollRecordBytes  = 8;
constexpr unsigned long kInterestOffset   = 4;
constexpr unsigned long kAnswerOffset     = 6;
constexpr unsigned long kPollRecordsLimit = 32;

unsigned short readiness(unsigned int number, unsigned int events) {
    const auto kOpen = LiveSlot(number);
    return kOpen.ok() ? PollOpen(kOpen.value().get(), events) : kPollNval;
}

/// Scans and rewrites the kernel-side copy of the caller's records;
/// the accessor moves the whole array across the user line once per
/// syscall, in and out, and the loop never touches ring 3.
unsigned long long scan_answers(unsigned char* base, unsigned long count) {
    unsigned long long ready = 0;
    for (unsigned long which = 0; which < count; ++which) {
        unsigned char* const kRecord = base + (which * kPollRecordBytes);
        unsigned int         number  = 0;
        unsigned short       events  = 0;
        base::CopyBytes(&number, kRecord, sizeof(number));
        base::CopyBytes(&events, kRecord + kInterestOffset, sizeof(events));
        unsigned short answer = 0;
        if (static_cast<int>(number) >= 0) {
            answer = readiness(number, events);
            if (answer != 0) {
                ++ready;
            }
        }
        base::CopyBytes(kRecord + kAnswerOffset, &answer, sizeof(answer));
    }
    return ready;
}

/// Puts the caller's own blocked set back after a masked wait.
void restore_blocked(proc::Task* self, const SigSet& restore, bool armed) {
    if (armed) {
        proc::FinishSignalWait(*self, restore);
    }
}

/// Reads the caller's records into kernel terms; false when the range
/// refuses the array.
bool gather_records(unsigned long long array, unsigned long count, unsigned char* slots) {
    if (count == 0) {
        return true;
    }
    return array != 0 && uaccess::CopyFromUser(slots, array, count * kPollRecordBytes);
}

/// Writes the scanned records back; a refused range on the way out
/// answers EFAULT after the wait itself already ran.
bool scatter_records(unsigned long long array, unsigned long count, const unsigned char* slots) {
    if (count == 0) {
        return true;
    }
    return uaccess::CopyToUser(array, slots, count * kPollRecordBytes);
}

}  // namespace

unsigned short PollOpen(const fs::OpenFile* file, unsigned int events) {
    unsigned short counter_ready = 0;
    if (PollEventCounter(file, events, counter_ready)) {
        return counter_ready;
    }
    if (file->ops == net::SocketOps()) {
        return static_cast<net::Socket*>(file->backend)
            ->poll_events(static_cast<unsigned short>(events));
    }
    if (file->ops != fs::PipeOps()) {
        unsigned int answer = events & (kPollIn | kPollOut);
        if ((answer & kPollIn) != 0U && !file->ops->readable(file->backend, file->node)) {
            answer &= ~kPollIn;
        }
        return static_cast<unsigned short>(answer);
    }
    const auto* const kPipe = static_cast<fs::Pipe*>(file->backend);
    if (file->node.opaque == nullptr) {
        unsigned int answer = 0;
        if ((events & kPollIn) != 0U && kPipe->has_bytes()) {
            answer |= kPollIn;
        }
        if (kPipe->writers_gone()) {
            answer |= kPollHup;
        }
        return static_cast<unsigned short>(answer);
    }
    if ((events & kPollOut) != 0U && kPipe->has_room()) {
        return static_cast<unsigned short>(kPollOut);
    }
    return 0;
}

long long HandlePpoll(const SyscallFrame& frame) {
    if (frame.rsi > kPollRecordsLimit) {
        return -kEinval;
    }
    const auto    kCount = static_cast<unsigned long>(frame.rsi);
    unsigned char slots[kPollRecordsLimit * kPollRecordBytes];
    base::SetBytes(slots, 0, sizeof(slots));
    if (!gather_records(frame.rdi, kCount, slots)) {
        return -kEfault;
    }

    unsigned long long deadline = ~0ULL;
    const long long    kParsed  = ReadWaitDeadline(frame.rdx, deadline);
    if (kParsed != 0) {
        return kParsed;
    }

    proc::Task* const kSelf    = CurrentTask();
    const SigSet      kRestore = kSelf->signals.blocked;
    bool              armed    = false;
    if (frame.r10 != 0) {
        unsigned long long words[1] = {};
        if (!uaccess::CopyFromUser(words, frame.r10, sizeof(SigSet))) {
            return -kEfault;
        }
        kSelf->signals.blocked = MaskableSignals(SigSet{words[0]});
        armed                  = true;
    }

    for (;;) {
        unsigned long long ready = 0;
        if (kCount != 0) {
            ready = scan_answers(slots, kCount);
        }
        const bool kDue    = ready != 0;
        const bool kSignal = proc::HasPendingSignal(*kSelf);
        const bool kExpire = MonotonicNanoseconds() >= deadline;
        if (kDue || kSignal || kExpire) {
            restore_blocked(kSelf, kRestore, armed);
            if (!scatter_records(frame.rdi, kCount, slots)) {
                return -kEfault;
            }
            if (kDue) {
                return static_cast<long long>(ready);
            }
            if (kSignal) {
                return -kEintr;
            }
            return 0;
        }
        proc::Scheduler::self().yield();
    }
}

long long HandlePoll(const SyscallFrame& frame) {
    if (frame.rsi > kPollRecordsLimit) {
        return -kEinval;
    }
    const auto    kCount = static_cast<unsigned long>(frame.rsi);
    unsigned char slots[kPollRecordsLimit * kPollRecordBytes];
    base::SetBytes(slots, 0, sizeof(slots));
    if (!gather_records(frame.rdi, kCount, slots)) {
        return -kEfault;
    }

    const unsigned long long kPollDeadline = MillisecondDeadline(static_cast<int>(frame.rdx));

    for (;;) {
        unsigned long long ready = 0;
        if (kCount != 0) {
            ready = scan_answers(slots, kCount);
        }
        const proc::Task* const kSelf   = CurrentTask();
        const bool              kDue    = ready != 0;
        const bool              kSignal = proc::HasPendingSignal(*kSelf);
        const bool              kExpire = MonotonicNanoseconds() >= kPollDeadline;
        if (kDue || kSignal || kExpire) {
            if (!scatter_records(frame.rdi, kCount, slots)) {
                return -kEfault;
            }
            if (kDue) {
                return static_cast<long long>(ready);
            }
            if (kSignal) {
                return -kEintr;
            }
            return 0;
        }
        proc::Scheduler::self().yield();
    }
}

}  // namespace cinux::syscall
