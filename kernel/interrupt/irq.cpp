/**
 * @file    irq.cpp
 * @brief   The one instance of the interrupt service.
 *
 * Part of the no-SSE island: the arch stubs reach dispatch on every
 * interrupt, and everything an iret returns through keeps off the
 * vector registers. No chip is named here — the backend arrives
 * erased, injected by the arch layer at bring-up.
 *
 * @author  Charliechen114514
 * @date    2026-10-02
 * @version 0.4
 * @since   0.1.0
 * @ingroup kernel_interrupt
 * @copyright Copyright (c) 2026
 */

#include "kernel/interrupt/irq.hpp"

#include "cinux/assert.hpp"
#include "cinux/result.hpp"
#include "kernel/interrupt/irq_config.hpp"

namespace cinux::interrupt {

void Irq::register_handler(IrqLine line, IrqHandler handler) {
    if (line.value < kIrqLineCount) {
        seats_[line.value] = handler;
    }
}

void Irq::enable_line(IrqLine line) {
    if (unmask_ != nullptr) {
        unmask_(chip_, line);
    }
}

void Irq::dispatch(IrqLine line) {
    if (line.value < kIrqLineCount && seats_[line.value] != nullptr) {
        seats_[line.value]();
    }
    if (ack_ != nullptr) {
        ack_(chip_, line);
    }
}

void Irq::init_message_range(unsigned int first, unsigned int count) {
    base::safety::Check(count <= kMessageVectorCount, "message pool larger than its table");
    message_first_ = first;
    message_count_ = count;
}

base::Result<MsiVector> Irq::seat_message(IrqHandler handler) {
    if (message_count_ == 0) {
        return base::KernelError::kNotSupported;
    }
    for (unsigned int index = 0; index < message_count_; ++index) {
        if (message_seats_[index] == nullptr) {
            message_seats_[index] = handler;
            return MsiVector{.value = message_first_ + index};
        }
    }
    return base::KernelError::kOutOfMemory;
}

void Irq::dispatch_message(MsiVector vector) {
    const unsigned long kOffset = vector.value - message_first_;
    if (vector.value >= message_first_ && kOffset < message_count_ &&
        message_seats_[kOffset] != nullptr) {
        message_seats_[kOffset]();
    }
}

}  // namespace cinux::interrupt
