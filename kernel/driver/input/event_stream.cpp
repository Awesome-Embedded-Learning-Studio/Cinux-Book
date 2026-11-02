#include "kernel/driver/input/event_stream.hpp"

#include <stdint.h>

#include "cinux/bit_ops/bitmask.hpp"
#include "cinux/memory.hpp"
#include "cinux/result.hpp"
#include "cinux/time.hpp"
#include "kernel/arch/x86_64/irq_guard.hpp"
#include "kernel/driver/input/input_config.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/time/tick.hpp"
#include "kernel/time/tick_config.hpp"

namespace cinux::driver {

namespace {
constexpr uint16_t kEvSyn      = 0;
constexpr uint16_t kSynReport  = 0;
constexpr uint16_t kSynDropped = 3;

/// One tick's worth of nanoseconds, the stamp's resolution.
constexpr unsigned long long kTickNanoseconds =
    base::time::ToNanoseconds(base::time::Seconds{1}).value / time::kTickHz.value;
constexpr unsigned long long kSecondNanoseconds =
    base::time::ToNanoseconds(base::time::Seconds{1}).value;
constexpr unsigned long long kMillisecondNanoseconds =
    base::time::ToNanoseconds(base::time::Milliseconds{1}).value;

InputEventWire stamp(uint16_t type, uint16_t code, int32_t value) {
    const unsigned long long kNanoseconds = time::Tick::self().since_boot() * kTickNanoseconds;
    return InputEventWire{
        .seconds      = kNanoseconds / kSecondNanoseconds,
        .microseconds = (kNanoseconds % kSecondNanoseconds) / kMillisecondNanoseconds,
        .type         = type,
        .code         = code,
        .value        = value,
    };
}

}  // namespace

void EventStream::publish(uint16_t type, uint16_t code, int32_t value) {
    const InputEventWire kRecord = stamp(type, code, value);
    const arch::IrqGuard kIrq;
    const auto           kGuard = gate_.guard();
    if (absolute_ && type == kEvAbs && code < axes_.size()) {
        axes_[code].value = value;
    }
    if (type == kEvKey && code < current_keys_.size()) {
        current_keys_[code] = value != 0;
    }
    if (!events_.push(kRecord)) {
        events_        = {};
        dropped_       = true;
        recovering_    = false;
        recovery_key_  = 0;
        recovery_axis_ = 0;
        events_.push(kRecord);
    }
}

void EventStream::publish_report() {
    publish(kEvSyn, kSynReport, 0);
}

base::Result<unsigned long> EventStream::read(void* sink, unsigned long count, const void* owner) {
    if (poll_hook_ != nullptr) {
        poll_hook_();
    }
    const unsigned long kRecords = count / kInputEventBytes;
    if (kRecords == 0) {
        return base::KernelError::kInvalidArgument;
    }
    const arch::IrqGuard kIrq;
    const auto           kGuard = gate_.guard();
    if (owner_ != nullptr && owner_ != owner) {
        return base::KernelError::kWouldBlock;
    }
    unsigned long handed = 0;
    while (handed < kRecords) {
        InputEventWire record{};
        if (dropped_) {
            record      = stamp(kEvSyn, kSynDropped, 0);
            dropped_    = false;
            recovering_ = true;
        } else if (recovering_) {
            while (recovery_key_ < delivered_keys_.size() && !delivered_keys_[recovery_key_]) {
                ++recovery_key_;
            }
            if (recovery_key_ == delivered_keys_.size() && absolute_ &&
                recovery_axis_ < axes_.size()) {
                record = stamp(kEvAbs, static_cast<uint16_t>(recovery_axis_),
                               axes_[recovery_axis_].value);
                ++recovery_axis_;
            } else if (recovery_key_ == delivered_keys_.size()) {
                record      = stamp(kEvSyn, kSynReport, 0);
                recovering_ = false;
            } else {
                record = stamp(kEvKey, static_cast<uint16_t>(recovery_key_++), 0);
            }
        } else if (!events_.pop(record)) {
            break;
        }
        if (record.type == kEvKey && record.code < delivered_keys_.size()) {
            delivered_keys_[record.code] = record.value != 0;
        }
        base::CopyBytes(static_cast<unsigned char*>(sink) + (handed * kInputEventBytes), &record,
                        kInputEventBytes);
        ++handed;
    }
    if (handed == 0) {
        return base::KernelError::kWouldBlock;
    }
    return handed * kInputEventBytes;
}

bool EventStream::poll(const void* owner) const {
    if (poll_hook_ != nullptr) {
        poll_hook_();
    }
    const arch::IrqGuard kIrq;
    const auto           kGuard = gate_.guard();
    return (owner_ == nullptr || owner_ == owner) &&
           (events_.count() != 0 || dropped_ || recovering_);
}

long long EventStream::answer(const void* owner, unsigned long long word,
                              unsigned long long argument) {
    const arch::IrqGuard kIrq;
    const auto           kGuard = gate_.guard();
    if (absolute_ && (word == kEvdevGetAbsX || word == kEvdevGetAbsY)) {
        const auto kAxis = word == kEvdevGetAbsX ? 0U : 1U;
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        base::CopyBytes(reinterpret_cast<void*>(argument), &axes_[kAxis], sizeof(InputAbsInfo));
        return 0;
    }
    if ((word & kEvdevGetKeysMask) == kEvdevGetKeys) {
        const auto kBytes  = base::bit::BitMask{word}.extract(kEvdevGetKeysSize);
        const auto kLimit  = InputKeyCodeBytes();
        const auto kCopied = kBytes < kLimit ? kBytes : kLimit;
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        auto*      bytes   = reinterpret_cast<unsigned char*>(argument);
        base::SetBytes(bytes, 0, kCopied);
        for (unsigned long index = 0; index < kCopied * 8; ++index) {
            if (current_keys_[index]) {
                bytes[index / 8] |=
                    base::bit::MaskBit<unsigned char>(static_cast<unsigned>(index % 8)).raw;
            }
        }
        return static_cast<long long>(kCopied);
    }
    if (word != kEvdevGrab) {
        return -syscall::kEnotty;
    }
    if (owner == nullptr) {
        return -syscall::kEinval;
    }
    if (argument != 0) {
        if (owner_ != nullptr) {
            return -syscall::kEbusy;
        }
        owner_ = owner;
    } else {
        if (owner_ != owner) {
            return -syscall::kEinval;
        }
        owner_ = nullptr;
    }
    if (grab_hook_ != nullptr) {
        grab_hook_(hook_word_, owner_ != nullptr);
    }
    return 0;
}

void EventStream::configure_absolute(int32_t maximum) {
    absolute_ = true;
    for (auto& axis : axes_) {
        axis.maximum = maximum;
    }
}

void EventStream::set_poll_hook(void (*hook)()) {
    poll_hook_ = hook;
}

void EventStream::set_grab_hook(InputGrabHook hook, void* context) {
    const arch::IrqGuard kIrq;
    const auto           kGuard = gate_.guard();
    grab_hook_                  = hook;
    hook_word_                  = context;
}

void EventStream::release_owner(const void* owner) {
    const arch::IrqGuard kIrq;
    const auto           kGuard = gate_.guard();
    if (owner_ == owner && owner != nullptr) {
        owner_ = nullptr;
        if (grab_hook_ != nullptr) {
            grab_hook_(hook_word_, false);
        }
    }
}

}  // namespace cinux::driver
