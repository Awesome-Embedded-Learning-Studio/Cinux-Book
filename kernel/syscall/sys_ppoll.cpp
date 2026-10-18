#include "kernel/syscall/sys_ppoll.hpp"

#include "cinux/result.hpp"
#include "kernel/driver/tty/tty.hpp"
#include "kernel/fs/open_file.hpp"
#include "kernel/fs/pipe.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/task.hpp"
#include "kernel/syscall/sys_clock.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"

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
    if (number < 3) {
        unsigned int answer = 0;
        if ((events & kPollIn) != 0U && driver::Tty::self().has_input()) {
            answer |= kPollIn;
        }
        if ((events & kPollOut) != 0U) {
            answer |= kPollOut;
        }
        return static_cast<unsigned short>(answer);
    }
    const base::Result<fs::OpenFile*> kOpen = LiveSlot(number);
    if (!kOpen.ok()) {
        return static_cast<unsigned short>(kPollNval);
    }
    const fs::OpenFile* const kFile = kOpen.value();
    if (kFile->ops != fs::PipeOps()) {
        return static_cast<unsigned short>(events & (kPollIn | kPollOut));
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

unsigned long long scan_answers(unsigned long long array, unsigned long count) {
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto* const        kBase = reinterpret_cast<unsigned char*>(array);
    unsigned long long ready = 0;
    for (unsigned long which = 0; which < count; ++which) {
        unsigned char* const kRecord = kBase + (which * kPollRecordBytes);
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        const auto           kNumber = *reinterpret_cast<const unsigned int*>(kRecord);
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        const auto  kEvents = *reinterpret_cast<const unsigned short*>(kRecord + kInterestOffset);
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        auto* const kAnswer = reinterpret_cast<unsigned short*>(kRecord + kAnswerOffset);
        if (static_cast<int>(kNumber) < 0) {
            *kAnswer = 0;
            continue;
        }
        *kAnswer = readiness(kNumber, kEvents);
        if (*kAnswer != 0) {
            ++ready;
        }
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
    if (timeout_user >= kUserAddressLimit || !UserRangeReadable(timeout_user, kTimespecBytes)) {
        return -kEfault;
    }
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    const auto* const kRecord = reinterpret_cast<const unsigned long long*>(timeout_user);
    if (kRecord[0] >= kNegativeSeconds || kRecord[1] >= kNsPerSecond) {
        return -kEinval;
    }
    const unsigned long long kNow   = MonotonicNanoseconds();
    const unsigned long long kSlack = ~0ULL - kNow;
    if (kRecord[1] > kSlack || kRecord[0] > ((kSlack - kRecord[1]) / kNsPerSecond)) {
        return 0;
    }
    deadline = (kNow + (kRecord[0] * kNsPerSecond)) + kRecord[1];
    return 0;
}

}  // namespace

long long HandlePpoll(const SyscallFrame& frame) {
    if (frame.rsi > kPollRecordsLimit) {
        return -kEinval;
    }
    const auto kCount = static_cast<unsigned long>(frame.rsi);
    if (kCount != 0) {
        const bool kUsable = frame.rdi != 0 && frame.rdi < kUserAddressLimit &&
                             UserRangeReadable(frame.rdi, kCount * kPollRecordBytes) &&
                             UserRangeWritable(frame.rdi, kCount * kPollRecordBytes);
        if (!kUsable) {
            return -kEfault;
        }
    }

    unsigned long long deadline = ~0ULL;
    const long long    kParsed  = deadline_from(frame.rdx, deadline);
    if (kParsed != 0) {
        return kParsed;
    }

    for (;;) {
        if (kCount != 0) {
            const unsigned long long kReady = scan_answers(frame.rdi, kCount);
            if (kReady != 0) {
                return static_cast<long long>(kReady);
            }
        }
        const proc::Task* const kSelf = CurrentTask();
        if (kSelf->signals.pending.has(~kSelf->signals.blocked)) {
            return -kEintr;
        }
        if (MonotonicNanoseconds() >= deadline) {
            return 0;
        }
        proc::Scheduler::self().yield();
    }
}

}  // namespace cinux::syscall
