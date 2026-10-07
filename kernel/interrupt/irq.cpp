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

}  // namespace cinux::interrupt
