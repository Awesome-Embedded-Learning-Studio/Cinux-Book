/**
 * @file    irq_stubs.cpp
 * @brief   Compiler-generated IRQ stubs feeding the interrupt table.
 *
 * Same mechanism as the exception family, second half of the vector
 * space: template instantiation bakes each vector number into its stub,
 * and IRQs never push an error code, so one bare signature serves all
 * sixteen. Part of the no-SSE island — everything an iret returns
 * through keeps off the vector registers, which is why the heartbeat
 * thunk lives here and on_interrupt is inline in tick.hpp.
 *
 * @author  Charliechen114514
 * @date    2026-10-02
 * @version 0.2
 * @since   0.1.0
 * @ingroup kernel_arch
 * @copyright Copyright (c) 2026
 */

#include "kernel/arch/x86_64/irq_stubs.hpp"

#include <utility>

#include "kernel/arch/x86_64/gdt.hpp"
#include "kernel/arch/x86_64/idt.hpp"
#include "kernel/arch/x86_64/isr.hpp"
#include "kernel/arch/x86_64/pic.hpp"
#include "kernel/interrupt/irq.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/time/tick.hpp"

namespace {

using cinux::arch::gdt::kSelectorCode;
using cinux::arch::idt::EncodeGate;
using cinux::arch::idt::InstallGate;
using cinux::arch::idt::kTypeInterruptGate;
using cinux::arch::isr::InterruptFrame;

constexpr unsigned int kFirstIrqVector = 32;
constexpr unsigned int kIrqCount       = 16;
constexpr unsigned int kTimerLine      = 0;

void heartbeat() {
    cinux::time::Tick::self().on_interrupt();
    cinux::proc::Scheduler::self().on_timer_tick();
}

template <unsigned int Vector>
__attribute__((interrupt)) void irq_entry([[maybe_unused]] InterruptFrame* frame) {
    cinux::interrupt::Irq::self().dispatch(
        cinux::interrupt::IrqLine{.value = Vector - kFirstIrqVector});
    cinux::proc::Scheduler::self().maybe_preempt();
}

void install(unsigned int vector, void (*handler)(InterruptFrame*)) {
    InstallGate(vector, EncodeGate(reinterpret_cast<unsigned long long>(handler), kSelectorCode, 0,
                                   kTypeInterruptGate));
}

template <unsigned int Vector>
void install_one() {
    install(Vector, &irq_entry<Vector>);
}

template <unsigned int... Vectors>
void install_all([[maybe_unused]] std::integer_sequence<unsigned int, Vectors...> sequence) {
    (install_one<Vectors + kFirstIrqVector>(), ...);
}

}  // namespace

namespace cinux::arch::irq {

void InstallIrqStubs() {
    cinux::interrupt::Irq::self().init(cinux::arch::Pic::self());
    cinux::interrupt::Irq::self().register_handler(cinux::interrupt::IrqLine{.value = kTimerLine},
                                                   heartbeat);
    install_all(std::make_integer_sequence<unsigned int, kIrqCount>{});
}

}  // namespace cinux::arch::irq
