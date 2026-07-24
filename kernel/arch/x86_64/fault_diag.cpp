/**
 * @file kernel/arch/x86_64/fault_diag.cpp
 * @brief debugcon first-fault diagnostics (F4-M4 GOTCHA#25 / F-VERIFY M6)
 *
 * Extracted from exception_handlers.cpp to keep that file under the 500-line
 * limit.  See fault_diag.hpp for the rationale (capture the faulting frame to
 * the always-survives debugcon channel before any path that could recurse).
 */
#include "kernel/arch/x86_64/fault_diag.hpp"

#include <stdint.h>

#include "kernel/arch/x86_64/idt.hpp"  // InterruptFrame full definition
#include "kernel/arch/x86_64/io.hpp"
#include "kernel/arch/x86_64/memory_layout.hpp"  // DIRECT_MAP_BASE
#include "kernel/arch/x86_64/paging.hpp"         // PageEntry
#include "kernel/arch/x86_64/paging_config.hpp"  // ADDR_MASK
#include "kernel/mm/pmm.hpp"                     // g_pmm.pte_count_load

namespace {

using cinux::arch::InterruptFrame;

constexpr uint16_t kDebugconPort = 0xE9;

void debugcon_str(const char* s) {
    while (*s != '\0') {
        cinux::io::io_outb(kDebugconPort, static_cast<uint8_t>(*s));
        ++s;
    }
}

void debugcon_hex64(uint64_t v) {
    static const char kHex[] = "0123456789abcdef";
    char              buf[19];  // "0x" + 16 hex digits + NUL
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 15; i >= 0; --i) {
        buf[2 + i] = kHex[v & 0xf];
        v >>= 4;
    }
    buf[18] = '\0';
    debugcon_str(buf);
}

// rdmsr with no %gs dependency (used to read GS_BASE/KERNEL_GS_BASE live, even
// when GS_BASE itself is non-canonical and would #GP on a %gs deref).
uint64_t read_msr_raw(uint32_t msr) {
    uint32_t low;
    uint32_t high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return (static_cast<uint64_t>(high) << 32) | low;
}

// %gs-safe first-#GP capture (F4-M4 M4-2-3, GOTCHA#25): a #GP whose root cause
// is a corrupt GS_BASE recurses handle_gp; this dumps the real faulting frame +
// the live GS/KERNEL_GS MSRs before anything touches %gs.
void dump_first_gp(const InterruptFrame* frame) {
    debugcon_str("\n>>> FIRST #GP rip=");
    debugcon_hex64(frame->rip);
    debugcon_str(" rsp=");
    debugcon_hex64(frame->rsp);
    debugcon_str(" cs=0x");
    debugcon_hex64(frame->cs & 0xffff);
    debugcon_str(" err=");
    debugcon_hex64(frame->error_code);
    debugcon_str(" rax(prev)=");
    debugcon_hex64(frame->rax);
    debugcon_str(" rbp=");
    debugcon_hex64(frame->rbp);
    uint64_t cr3;
    __asm__ volatile("movq %%cr3, %0" : "=r"(cr3));
    debugcon_str(" cr3=");
    debugcon_hex64(cr3);
    debugcon_str(" gs_base=");
    debugcon_hex64(read_msr_raw(0xC0000101));
    debugcon_str(" kgs_base=");
    debugcon_hex64(read_msr_raw(0xC0000102));
    debugcon_str(" <<<\n");
}

// F-VERIFY M6-1: first-#PF capture.  Decodes the PF error code (P/W/U/RSV/I) so
// a glance at debug.log says CoW/permission vs demand/segfault vs user/supervisor.
void dump_first_pf(const InterruptFrame* frame, uint64_t cr2) {
    debugcon_str("\n>>> FIRST #PF rip=");
    debugcon_hex64(frame->rip);
    debugcon_str(" rsp=");
    debugcon_hex64(frame->rsp);
    debugcon_str(" cr2=");
    debugcon_hex64(cr2);
    debugcon_str(" err=0x");
    debugcon_hex64(frame->error_code);
    debugcon_str(" [");
    debugcon_str((frame->error_code & 1) ? "P" : "!P");
    debugcon_str((frame->error_code & 2) ? " W" : " R");
    debugcon_str((frame->error_code & 4) ? " U" : " S");
    if (frame->error_code & 8) {
        debugcon_str(" RSV");
    }
    if (frame->error_code & 16) {
        debugcon_str(" I");
    }
    debugcon_str("]");
    debugcon_str(" rbp=");
    debugcon_hex64(frame->rbp);
    uint64_t cr3;
    __asm__ volatile("movq %%cr3, %0" : "=r"(cr3));
    debugcon_str(" cr3=");
    debugcon_hex64(cr3);
    debugcon_str(" <<<\n");
}

// F-VERIFY M6-2: lock-free walk of the faulting PTE (read CR3, descend
// PML4->PDPT->PD->PT through kernel page-table pages only -- no locks, no SMAP
// bypass) to print the backing phys + its pte_count.  A cross-core CoW UAF shows
// pte_count=0 or a stale phys here.
void dump_cow_fail_diagnostic_impl(uint64_t fault_addr) {
    uint64_t cr3;
    __asm__ volatile("movq %%cr3, %0" : "=r"(cr3));
    auto*     t = reinterpret_cast<const cinux::arch::PageEntry*>(cinux::arch::DIRECT_MAP_BASE +
                                                                  (cr3 & cinux::arch::ADDR_MASK));
    uint64_t  phys    = 0;
    const int shft[4] = {39, 30, 21, 12};
    for (int level = 0; level < 4; level++) {
        const cinux::arch::PageEntry& e = t[(fault_addr >> shft[level]) & 0x1FF];
        if (!e.is_present()) {
            break;
        }
        if (e.huge || level == 3) {
            phys = e.phys_addr();  // huge leaf or PT leaf
            break;
        }
        t = reinterpret_cast<const cinux::arch::PageEntry*>(cinux::arch::DIRECT_MAP_BASE +
                                                            e.phys_addr());
    }
    debugcon_str("\n>>> CoW-FAIL cr2=");
    debugcon_hex64(fault_addr);
    debugcon_str(" phys=");
    debugcon_hex64(phys);
    debugcon_str(" pte_count=");
    debugcon_hex64(static_cast<uint64_t>(cinux::mm::g_pmm.pte_count_load(phys)));
    debugcon_str(" refcount=");
    debugcon_hex64(static_cast<uint64_t>(cinux::mm::g_pmm.refcount_load(phys)));
    debugcon_str(" <<<\n");
}

}  // namespace

void capture_first_gp(const cinux::arch::InterruptFrame* frame) {
    static bool dumped = false;
    if (!dumped) {
        dumped = true;
        dump_first_gp(frame);
    }
}

void capture_first_pf(const cinux::arch::InterruptFrame* frame, uint64_t cr2) {
    static bool dumped = false;
    if (!dumped) {
        dumped = true;
        dump_first_pf(frame, cr2);
    }
}

void dump_cow_fail_diagnostic(uint64_t fault_addr) {
    dump_cow_fail_diagnostic_impl(fault_addr);
}
