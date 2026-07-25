/**
 * @file kernel/drivers/pit/pit.cpp
 * @brief PIT (Intel 8254) driver implementation
 *
 * Configures PIT channel 0 in square-wave mode, maintains a tick
 * counter, and provides uptime tracking.  The IRQ0 handler prints
 * a "[TICK] uptime: Ns" message once per second.
 */

#include "pit.hpp"

#include <stdint.h>

#include "kernel/arch/x86_64/idt.hpp"
#include "kernel/arch/x86_64/io.hpp"
#include "kernel/arch/x86_64/irq_backend.hpp"
#include "kernel/arch/x86_64/pic.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/signal.hpp"  // itimer_real_tick (ITIMER_REAL -> SIGALRM)

using cinux::arch::InterruptFrame;
using cinux::arch::PIC;
using cinux::io::io_outb;
using cinux::lib::kprintf;

namespace cinux::drivers {

// ============================================================
// Static storage
// ============================================================

lib::Atomic<uint64_t> PIT::tick_count_{0};
uint32_t              PIT::freq_hz_ = 100;

// ============================================================
// PIT::init() -- configure channel 0 as square-wave generator
// ============================================================

void PIT::init(uint32_t freq_hz) {
    // Store the frequency for uptime calculations
    freq_hz_ = freq_hz;

    // Calculate the divisor: base_clock / desired_frequency
    // Clamp to 16-bit range [1, 65535]
    uint32_t divisor = PitHW::BASE_FREQ / freq_hz;
    if (divisor > 65535) {
        divisor = 65535;
    }
    if (divisor == 0) {
        divisor = 1;
    }

    // Command byte 0x34:
    //   0x30 = channel 0, LSB-then-MSB access mode
    //   0x04 = rate generator (mode 2)
    //   0x00 = binary counter (not BCD)
    // Total: 0x34
    // Mode 2 (rate generator), not mode 3 (square wave): mode 3's counter
    // reaches terminal count twice per output period (it decrements by 2 and
    // toggles at 0), and QEMU raises IRQ0 at each terminal count -- so mode 3
    // fires IRQ0 at 2x the configured frequency (a 100 Hz timer ran at 200 Hz,
    // surfacing as busybox ping's 1 s interval firing every ~0.5 s).  Mode 2
    // decrements by 1 and pulses once per period -> exactly freq_hz_ IRQs/s.
    // This is the mode Linux uses for the periodic clock source.
    io_outb(PitHW::COMMAND,
            PitHW::CMD_CHANNEL_0 | PitHW::CMD_LSB_MSB | PitHW::CMD_MODE_2 | PitHW::CMD_BINARY);

    // Write divisor: low byte first, then high byte
    io_outb(PitHW::CHANNEL_0, static_cast<uint8_t>(divisor & 0xFF));
    io_outb(PitHW::CHANNEL_0, static_cast<uint8_t>((divisor >> 8) & 0xFF));

    // Reset tick counter
    tick_count_ = 0;

    kprintf("[PIT] Initialised at %u Hz (divisor=%u)\n", freq_hz_, divisor);
}

// ============================================================
// PIT::irq0_handler() -- called from ISR stub on every tick
// ============================================================

void PIT::irq0_handler(InterruptFrame* /*frame*/) {
    // Increment the global tick counter
    tick_count_.fetch_add(1, lib::MemoryOrder::Relaxed);
    // Scheduler::tick() accounts timer state only; it must not switch tasks
    // inline from IRQ context.  EOI after the tick keeps irq0 non-reentrant
    // while timer_queue_tick() runs.
    cinux::proc::Scheduler::tick();
    // ITIMER_REAL wall-clock advance: the PIT is the global tick source (BSP;
    // the AP's LAPIC timer drives preemption only), so decrement every task's
    // itimer here and queue SIGALRM on expiry.  Runs before EOI so a periodic
    // 1 s timer (busybox ping) fires ~every 100 ticks at 100 Hz.
    cinux::proc::itimer_real_tick(1'000'000'000ULL / freq_hz_);
    cinux::arch::irq_eoi(0);
}

// ============================================================
// PIT::get_ticks() -- return the current tick count
// ============================================================

uint64_t PIT::get_ticks() {
    return tick_count_.load(lib::MemoryOrder::Relaxed);
}

// ============================================================
// PIT::get_uptime_ms() -- return uptime in milliseconds
// ============================================================

uint64_t PIT::get_uptime_ms() {
    // (tick_count * 1000) / freq_hz gives milliseconds
    return (tick_count_.load(lib::MemoryOrder::Relaxed) * 1000) / freq_hz_;
}

// ============================================================
// PIT::freq_hz() -- return configured frequency
// ============================================================

uint32_t PIT::freq_hz() {
    return freq_hz_;
}

}  // namespace cinux::drivers

// ============================================================
// C-linkage bridge: called from irq0_stub in interrupts.S
// ============================================================

extern "C" void pit_irq0_handler(cinux::arch::InterruptFrame* frame) {
    cinux::drivers::PIT::irq0_handler(frame);
}
