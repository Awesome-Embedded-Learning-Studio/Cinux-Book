#include "cinux/ptr.hpp"
#include "kernel/arch/x86_64/context.hpp"
#include "kernel/arch/x86_64/halt.hpp"
#include "kernel/arch/x86_64/isr.hpp"
#include "kernel/arch/x86_64/page_entry.hpp"
#include "kernel/arch/x86_64/registers.hpp"
#include "kernel/boot/print.hpp"
#include "kernel/debug/trace.hpp"
#include "kernel/mm/layout.hpp"
#include "kernel/mm/page_walk.hpp"
#include "kernel/proc/fpu_gate.hpp"

namespace cinux::arch::isr {

namespace {

PageFaultResolver g_page_fault_resolver = nullptr;

PageFaultDiagnoser g_page_fault_diagnoser = nullptr;

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

void report_page_fault(unsigned long long address, unsigned long long error_code) {
    const char* const kAccess = (error_code & kFaultWasWrite) != 0 ? "write" : "read";
    const char* const kOrigin = (error_code & kFaultFromUser) != 0 ? "user" : "kernel";
    const char* const kCause  = (error_code & kFaultProtection) != 0 ? "protection" : "not-present";
    print::Println("[kern]  cr2=%X err=%X (%s, %s, %s)", address, error_code, kAccess, kOrigin,
                   kCause);
    using cinux::arch::page::Entry;
    using cinux::mm::walk::IndexRange;
    using cinux::mm::walk::SlotIndex;
    using cinux::mm::walk::WalkLevel;
    const unsigned long kRoot = cinux::arch::ReadCr3();
    print::Println("[kern]  root=%X (cr3 at fault time)", kRoot);
    unsigned long     table          = kRoot;
    const char* const kLevelNames[4] = {"pml4e", "pdpte", "pde", "pte"};
    for (unsigned int depth = 0; depth < 4; ++depth) {
        const auto         kLevel   = static_cast<WalkLevel>(depth);
        const Entry* const kSlot    = cinux::base::PtrAt<Entry>(table) + SlotIndex(address, kLevel);
        const bool         kPresent = kSlot->has(cinux::arch::page::kPresent);
        const bool         kWrite   = kSlot->has(cinux::arch::page::kWritable);
        const bool         kUser    = kSlot->has(cinux::arch::page::kUser);
        const bool         kLarge   = kSlot->has(cinux::arch::page::kLarge);
        print::Println("[kern]  %s[%u]=%X %s%s%s%s%s", kLevelNames[depth],
                       static_cast<unsigned>(SlotIndex(address, kLevel)), kSlot->raw,
                       kPresent ? "P" : "-", kWrite ? "W" : "-", kUser ? "U" : "-",
                       kLarge ? "L" : "", kPresent ? "" : " walk ends here");
        if (!kPresent) {
            return;
        }
        if (depth == 3 || kLarge) {
            return;
        }
        table = cinux::mm::DirectMapVirt(cinux::mm::walk::EntryTarget(*kSlot));
    }
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

void InstallPageFaultResolver(PageFaultResolver resolver) {
    g_page_fault_resolver = resolver;
}

void InstallPageFaultDiagnoser(PageFaultDiagnoser diagnoser) {
    g_page_fault_diagnoser = diagnoser;
}

void ReportFault(unsigned long long vector, InterruptFrame& frame, unsigned long long error_code) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
    FpuImage interrupted_fpu;
    SaveFpu(interrupted_fpu);
    EnterFpuGate();
    if (vector == kPageFaultVector && g_page_fault_resolver != nullptr &&
        g_page_fault_resolver(ReadCr2(), error_code)) {
        ExitFpuGate();
        RestoreFpu(interrupted_fpu);
        return;
    }
    if (vector == kPageFaultVector && g_page_fault_diagnoser != nullptr) {
        g_page_fault_diagnoser(ReadCr2(), error_code);
    }
    if (try_recover(vector, frame)) {
        ExitFpuGate();
        RestoreFpu(interrupted_fpu);
        return;
    }
    cinux::print::Println("[kern] exception #%u %s", vector,
                          name_of(static_cast<unsigned>(vector)));
    cinux::print::Println("[kern]  rip=%X cs=%X rflags=%X rsp=%X err=%X", frame.rip, frame.cs,
                          frame.rflags, frame.rsp, error_code);
    if (vector == kPageFaultVector) {
        report_page_fault(ReadCr2(), error_code);
    }
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
