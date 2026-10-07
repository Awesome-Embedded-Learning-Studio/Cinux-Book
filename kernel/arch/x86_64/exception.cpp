#include "cinux/ptr.hpp"
#include "kernel/arch/x86_64/halt.hpp"
#include "kernel/arch/x86_64/isr.hpp"
#include "kernel/boot/print.hpp"
#include "kernel/debug/trace.hpp"

namespace cinux::arch::isr {

namespace {

unsigned long long faulting_frame_base() {
    unsigned long long reporter_frame = 0;  // NOLINT(misc-const-correctness)
    __asm__ volatile("movq %%rbp, %0" : "=r"(reporter_frame));
    const auto* stub_frame = cinux::base::PtrAt<const unsigned long long>(reporter_frame);
    return stub_frame[0];
}

const char* const kExceptionNames[32] = {
    "Division Error",
    "Debug",
    "NMI",
    "Breakpoint",
    "Overflow",
    "Bound Range Exceeded",
    "Invalid Opcode",
    "Device Not Available",
    "Double Fault",
    "Coprocessor Segment Overrun",
    "Invalid TSS",
    "Segment Not Present",
    "Stack-Segment Fault",
    "General Protection Fault",
    "Page Fault",
    "Reserved",
    "x87 Floating-Point Exception",
    "Alignment Check",
    "Machine Check",
    "SIMD Floating-Point Exception",
    "Virtualization Exception",
    "Control Protection",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
};

const char* name_of(unsigned int vector) {
    return vector < 32 ? kExceptionNames[vector] : "Unknown";
}

struct RecoveryPlan {
    unsigned long long vector;
    unsigned long long skip_bytes;
    bool               armed;
};

RecoveryPlan g_recovery{};

constexpr unsigned long long kNoFaultRecovered = 256;

unsigned long long g_recovered = kNoFaultRecovered;

bool try_recover(unsigned long long vector, InterruptFrame& frame) {
    if (!g_recovery.armed || vector != g_recovery.vector) {
        return false;
    }
    frame.rip += g_recovery.skip_bytes;
    g_recovery.armed = false;
    g_recovered      = vector;
    return true;
}

}  // namespace

void ReportFault(unsigned long long vector, InterruptFrame& frame, unsigned long long error_code) {
    if (try_recover(vector, frame)) {
        return;
    }
    cinux::print::Println("[kern] exception #%u %s", vector,
                          name_of(static_cast<unsigned>(vector)));
    cinux::print::Println("[kern]  rip=%X cs=%X rflags=%X rsp=%X err=%X", frame.rip, frame.cs,
                          frame.rflags, frame.rsp, error_code);
    cinux::debug::TraceFrom(faulting_frame_base());
    cinux::arch::Halt();
}

bool ArmRecoverableFault(unsigned long long vector, unsigned long long skip_bytes) {
    if (g_recovery.armed) {
        return false;
    }
    g_recovery  = RecoveryPlan{.vector = vector, .skip_bytes = skip_bytes, .armed = true};
    g_recovered = kNoFaultRecovered;
    return true;
}

unsigned long long TakeRecoveredVector() {
    const unsigned long long kVector = g_recovered;
    g_recovered                      = kNoFaultRecovered;
    return kVector;
}

}  // namespace cinux::arch::isr
