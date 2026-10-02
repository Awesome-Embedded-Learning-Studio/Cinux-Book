#include "kernel/arch/x86_64/isr.hpp"
#include "kernel/boot/console.hpp"
#include "kernel/boot/print.hpp"

namespace cinux::arch::isr {

namespace {

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

const char* name_of(unsigned vector) {
    return vector < 32 ? kExceptionNames[vector] : "Unknown";
}

}  // namespace

void ReportFault(unsigned long long vector, const InterruptFrame& frame,
                 unsigned long long error_code) {
    cinux::print::Println("[kern] exception #%u %s", vector,
                          name_of(static_cast<unsigned>(vector)));
    cinux::print::Println("[kern]  rip=%X cs=%X rflags=%X rsp=%X err=%X", frame.rip, frame.cs,
                          frame.rflags, frame.rsp, error_code);
    cinux::console::Halt();
}

}  // namespace cinux::arch::isr
