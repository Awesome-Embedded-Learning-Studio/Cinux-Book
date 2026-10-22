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

#include <atomic>
#include <utility>

#include "kernel/arch/x86_64/context.hpp"
#include "kernel/arch/x86_64/gdt.hpp"
#include "kernel/arch/x86_64/idt.hpp"
#include "kernel/arch/x86_64/isr.hpp"
#include "kernel/arch/x86_64/per_cpu.hpp"
#include "kernel/arch/x86_64/pic.hpp"
#include "kernel/arch/x86_64/smp_config.hpp"
#include "kernel/arch/x86_64/vector_config.hpp"
#include "kernel/driver/acpi/acpi.hpp"
#include "kernel/driver/acpi/madt.hpp"
#include "kernel/driver/apic/io_apic.hpp"
#include "kernel/driver/apic/local_apic.hpp"
#include "kernel/interrupt/irq.hpp"
#include "kernel/interrupt/irq_config.hpp"
#include "kernel/proc/fpu_gate.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/time/tick.hpp"

namespace {

using cinux::arch::gdt::kSelectorCode;
using cinux::arch::idt::EncodeGate;
using cinux::arch::idt::InstallGate;
using cinux::arch::idt::kTypeInterruptGate;
using cinux::arch::isr::InterruptFrame;

using cinux::arch::irq::kFirstIrqVector;
using cinux::arch::irq::kSpuriousVector;
constexpr unsigned int kIrqCount  = 16;
constexpr unsigned int kTimerLine = 0;

__attribute__((interrupt)) void spurious_entry([[maybe_unused]] InterruptFrame* frame) {}

void heartbeat() {
    cinux::time::Tick::self().on_interrupt();
    cinux::proc::Scheduler::self().on_timer_tick();
}

template <unsigned int Vector>
__attribute__((interrupt)) void irq_entry([[maybe_unused]] InterruptFrame* frame) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
    cinux::arch::FpuImage interrupted_fpu;
    cinux::arch::SaveFpu(interrupted_fpu);
    EnterFpuGate();
    cinux::interrupt::Irq::self().dispatch(
        cinux::interrupt::IrqLine{.value = Vector - kFirstIrqVector});
    cinux::proc::Scheduler::self().maybe_preempt();
    ExitFpuGate();
    cinux::arch::RestoreFpu(interrupted_fpu);
}

void install(unsigned int vector, void (*handler)(InterruptFrame*)) {
    InstallGate(vector, EncodeGate(reinterpret_cast<unsigned long long>(handler), kSelectorCode, 0,
                                   kTypeInterruptGate));
}

template <unsigned int Vector>
__attribute__((interrupt)) void local_entry([[maybe_unused]] InterruptFrame* frame) {
    cinux::arch::FpuImage interrupted_fpu{};
    cinux::arch::SaveFpu(interrupted_fpu);
    EnterFpuGate();
    cinux::driver::LocalApic::self().eoi();
    if constexpr (Vector == cinux::arch::smp::kTimerVector) {
        cinux::arch::per_cpu::PerCpu::self().local_timer_interrupts.fetch_add(
            1, std::memory_order_relaxed);
        cinux::proc::Scheduler::self().on_timer_tick();
    } else {
        cinux::arch::per_cpu::PerCpu::self().reschedule_interrupts.fetch_add(
            1, std::memory_order_relaxed);
    }
    cinux::proc::Scheduler::self().maybe_preempt();
    ExitFpuGate();
    cinux::arch::RestoreFpu(interrupted_fpu);
}

template <unsigned int Vector>
void install_one() {
    install(Vector, &irq_entry<Vector>);
}

template <unsigned int... Vectors>
void install_all([[maybe_unused]] std::integer_sequence<unsigned int, Vectors...> sequence) {
    (install_one<Vectors + kFirstIrqVector>(), ...);
}

template <unsigned int Vector>
__attribute__((interrupt)) void message_entry([[maybe_unused]] InterruptFrame* frame) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
    cinux::arch::FpuImage interrupted_fpu;
    cinux::arch::SaveFpu(interrupted_fpu);
    EnterFpuGate();
    cinux::interrupt::Irq::self().dispatch_message(cinux::interrupt::MsiVector{.value = Vector});
    cinux::driver::LocalApic::self().eoi();
    cinux::proc::Scheduler::self().maybe_preempt();
    ExitFpuGate();
    cinux::arch::RestoreFpu(interrupted_fpu);
}

template <unsigned int Vector>
void install_message_one() {
    install(Vector, &message_entry<Vector>);
}

template <unsigned int... Vectors>
void install_message_all(
    [[maybe_unused]] std::integer_sequence<unsigned int, Vectors...> sequence) {
    (install_message_one<Vectors + cinux::arch::irq::kFirstMessageVector>(), ...);
}

}  // namespace

namespace cinux::arch::irq {

void InstallLocalInterrupts() {
    install(smp::kTimerVector, local_entry<smp::kTimerVector>);
    install(smp::kRescheduleVector, local_entry<smp::kRescheduleVector>);
}

bool InstallApicBackend() {
    const auto kTopology = driver::acpi::FindInterruptTopology(driver::acpi::FindRootPointer());
    if (!driver::LocalApic::self().init(kTopology.local_apic_address) ||
        !driver::IoApic::self().init(kTopology)) {
        return false;
    }
    install(kSpuriousVector, spurious_entry);
    driver::LocalApic::self().enable(kSpuriousVector);
    interrupt::Irq::self().init(driver::IoApic::self());
    interrupt::Irq::self().init_message_range(irq::kFirstMessageVector,
                                              interrupt::kMessageVectorCount);
    install_message_all(std::make_integer_sequence<unsigned int, interrupt::kMessageVectorCount>{});
    return true;
}

void InstallIrqStubs() {
    cinux::interrupt::Irq::self().init(cinux::arch::Pic::self());
    cinux::interrupt::Irq::self().register_handler(cinux::interrupt::IrqLine{.value = kTimerLine},
                                                   heartbeat);
    install_all(std::make_integer_sequence<unsigned int, kIrqCount>{});
}

}  // namespace cinux::arch::irq
