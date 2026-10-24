#include "kernel/syscall/sys_ppoll.hpp"

#include "cinux/memory.hpp"
#include "cinux/result.hpp"
#include "cinux/shared.hpp"
#include "kernel/fs/open_file.hpp"
#include "kernel/fs/pipe.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/signals.hpp"
#include "kernel/proc/task.hpp"
#include "kernel/syscall/signal.hpp"
#include "kernel/syscall/sys_clock.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"
#include "kernel/syscall/uaccess.hpp"

namespace cinux::syscall {

namespace {

constexpr unsigned int kPollIn   = 0x0001U;
constexpr unsigned int kPollOut  = 0x0004U;
constexpr unsigned int kPollHup  = 0x0010U;
constexpr unsigned int kPollNval = 0x0020U;

constexpr unsigned long kPollRecordBytes  = 8;
constexpr unsigned long kInterestOffset   = 4;
constexpr unsigned long kAnswerOffset     = 6;
constexpr unsigned long kPollRecordsLimit = 32;

constexpr unsigned long long kNsPerSecond     = 1'000'000'000ULL;
constexpr unsigned long long kTimespecBytes   = 16;
constexpr unsigned long long kNegativeSeconds = 0x8000000000000000ULL;

unsigned short readiness(unsigned int number, unsigned int events) {
    const base::Result<base::Shared<fs::OpenFile>> kOpen = LiveSlot(number);
    if (!kOpen.ok()) {
        return static_cast<unsigned short>(kPollNval);
    }
    const fs::OpenFile* const kFile = kOpen.value().get();
    if (kFile->ops != fs::PipeOps()) {
        unsigned int answer = events & (kPollIn | kPollOut);
        if ((answer & kPollIn) != 0U && !kFile->ops->readable(kFile->backend, kFile->node)) {
            answer &= ~kPollIn;
        }
        return static_cast<unsigned short>(answer);
    }
    const auto* const kPipe = static_cast<fs::Pipe*>(kFile->backend);
    if (kFile->node.opaque == nullptr) {
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

/**
 * @brief     Reads the caller's timeout record into an expiry deadline.
 *
 * @param[in] timeout_user   The user address of the timespec, maybe zero.
 * @param[out] deadline      The monotonic expiry; stays at the all-ones
 *                           word when the caller named no deadline.
 * @return    Zero when the deadline is settled, or a negative errno.
 * @note      A zero address means wait forever, so the deadline word
 *            stays all-ones and the loop below never sees time run
 *            out. The same answer covers a record so long the
 *            monotonic clock cannot count it: waiting past the clock's
 *            range is waiting forever to this kernel.
 */
long long deadline_from(unsigned long long timeout_user, unsigned long long& deadline) {
    if (timeout_user == 0) {
        return 0;
    }
    unsigned long long record[2] = {};
    if (!uaccess::CopyFromUser(record, timeout_user, kTimespecBytes)) {
        return -kEfault;
    }
    if (record[0] >= kNegativeSeconds || record[1] >= kNsPerSecond) {
        return -kEinval;
    }
    const unsigned long long kNow   = MonotonicNanoseconds();
    const unsigned long long kSlack = ~0ULL - kNow;
    if (record[1] > kSlack || record[0] > ((kSlack - record[1]) / kNsPerSecond)) {
        return 0;
    }
    deadline = (kNow + (record[0] * kNsPerSecond)) + record[1];
    return 0;
}

/// Puts the caller's own blocked set back after a masked wait.
void restore_blocked(proc::Task* self, const SigSet& restore, bool armed) {
    if (armed) {
        self->signals.blocked = restore;
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
    const long long    kParsed  = deadline_from(frame.rdx, deadline);
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
        kSelf->signals.blocked = SigSet{words[0]};
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

    unsigned long long deadline = ~0ULL;
    const auto         kMillis  = static_cast<long long>(static_cast<long>(frame.rdx));
    if (kMillis >= 0) {
        const unsigned long long kNow   = MonotonicNanoseconds();
        const unsigned long long kSlack = ~0ULL - kNow;
        const unsigned long long kWant  = static_cast<unsigned long long>(kMillis) * 1000000ULL;
        if (kWant <= kSlack) {
            deadline = kNow + kWant;
        }
    }

    for (;;) {
        unsigned long long ready = 0;
        if (kCount != 0) {
            ready = scan_answers(slots, kCount);
        }
        const proc::Task* const kSelf   = CurrentTask();
        const bool              kDue    = ready != 0;
        const bool              kSignal = proc::HasPendingSignal(*kSelf);
        const bool              kExpire = MonotonicNanoseconds() >= deadline;
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
