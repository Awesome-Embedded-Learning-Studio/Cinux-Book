/**
 * @file kernel/drivers/apic/local_apic.hpp
 * @brief Local APIC (xAPIC MMIO) driver (F4-M2)
 *
 * Drives the per-CPU Local APIC through its 4 KB MMIO window.  xAPIC mode
 * (MMIO) is mandatory because QEMU's default qemu64 CPU has no x2APIC; the
 * MSR-based x2APIC path is deferred.
 *
 * The MMIO window is uncached (FLAG_PCD): APIC registers are memory-mapped
 * I/O, not RAM, so cache-enabled access (e.g. via the direct map) is undefined.
 * init() maps the window inside KMEM_MMIO (offset past the AHCI BAR5 mapping);
 * bind() attaches to an already-mapped (or mock) base for testing.
 *
 * M2-1 provides the driver only (read/write/enable/eoi/id/version/error).
 * Switching the system from the 8259 PIC to the APIC happens in M2-3.
 *
 * Namespace: cinux::drivers::apic
 */

#pragma once

#include <stdint.h>

namespace cinux::drivers::apic {

// Local APIC MMIO register offsets (xAPIC, byte offsets into the 4 KB window).
constexpr uint32_t kRegId           = 0x020;  ///< Local APIC ID (bits 24-31)
constexpr uint32_t kRegVersion      = 0x030;  ///< version (bits 0-7)
constexpr uint32_t kRegTaskPriority = 0x080;
constexpr uint32_t kRegEoi          = 0x0B0;  ///< write 0 to end interrupt
constexpr uint32_t kRegSpurious     = 0x0F0;  ///< spurious vector + enable bit 8
constexpr uint32_t kRegErrorStatus  = 0x280;
constexpr uint32_t kRegLvtTimer     = 0x320;
constexpr uint32_t kRegTimerInit    = 0x380;
constexpr uint32_t kRegTimerCurrent = 0x390;
constexpr uint32_t kRegTimerDivide  = 0x3E0;

/// Spurious Interrupt Vector Register bit 8: APIC software enable.
constexpr uint32_t kSvrEnable = 1u << 8;

// Interrupt Command Register (ICR) -- used by IPIs (F4-M3 P2-1).  64-bit,
// split across two 32-bit MMIO words: high holds the destination APIC ID
// (bits 24-31, physical mode), low holds the vector + delivery mode + flags.
constexpr uint32_t kRegIcrLow  = 0x300;  ///< ICR low: vector / delivery mode / flags
constexpr uint32_t kRegIcrHigh = 0x310;  ///< ICR high: dest APIC ID in bits 24-31

/// ICR low bit 12: delivery status (0 = Idle, 1 = Send Pending).  Read-only;
/// polled before issuing the next IPI so we never overwrite a pending send.
constexpr uint32_t kIcrDeliveryStatus = 1u << 12;
/// ICR low bit 14: level = assert (required for INIT/SIPI delivery).
constexpr uint32_t kIcrLevelAssert    = 1u << 14;

// Delivery modes (ICR low bits 8-10).
constexpr uint32_t kIcrModeFixed = 0u << 8;  ///< normal fixed-vector IPI
constexpr uint32_t kIcrModeInit  = 5u << 8;  ///< INIT (AP startup sequence)
constexpr uint32_t kIcrModeSipi  = 6u << 8;  ///< Startup (SIPI; vector = page nr)

class LocalAPIC {
public:
    /// Attach to an already-mapped (or mock) 4 KB MMIO window.
    void bind(volatile uint32_t* mmio_base);

    /// Map the Local APIC MMIO window (FLAG_PCD, uncached) inside KMEM_MMIO and
    /// bind to it.  Used in production; tests call bind() with a mock page.
    /// @return true on success.
    bool init(uint64_t mmio_phys);

    /// Read/write a 32-bit register at byte offset @p off.
    uint32_t read(uint32_t off) const;
    void     write(uint32_t off, uint32_t value);

    uint32_t id() const;       ///< APIC ID (bits 24-31 of kRegId)
    uint32_t version() const;  ///< version (bits 0-7 of kRegVersion)

    /// Enable the APIC: SVR = spurious_vector | kSvrEnable.
    void enable(uint8_t spurious_vector);
    void disable();

    void     eoi();  ///< End Of Interrupt (write 0 to EOI register).
    uint32_t error_status();
    void     clear_error();

    // ---- LAPIC timer (per-CPU periodic preemption, F5-M5 -smp) ----
    // BSP is preempted by the legacy PIT; the PIT reaches the BSP only (the
    // IOAPIC redirect in switch_to_apic targets the BSP), so APs would run
    // without any periodic timer -> no preemption -> a task that spin-waits
    // (e.g. shell on stdin) deadlocks its AP.  This programs the LAPIC's
    // built-in timer in periodic mode so each AP self-preempts at the same
    // Scheduler::tick() path the PIT drives on the BSP.
    /// Program the LAPIC timer: periodic, unmasked, @p vector, decrementing at
    /// (APIC clock / divide) from @p init_count.  divide_bits is the raw 3-bit
    /// divide code (e.g. 0x3 = /16); init_count is the reload value.
    void setup_periodic_timer(uint8_t vector, uint8_t divide_bits, uint32_t init_count);

    // ---- Inter-Processor Interrupts (F4-M3 P2-1) ----
    // All IPIs use physical destination mode + edge trigger.  Each waits for
    // the ICR delivery status to go Idle before writing, so a previous IPI is
    // never overwritten mid-send.
    /// Send a fixed-vector IPI carrying @p vector to @p dest_apic_id.
    void send_ipi(uint8_t dest_apic_id, uint8_t vector);
    /// Send an INIT IPI to @p dest_apic_id (first step of AP startup).
    void send_init(uint8_t dest_apic_id);
    /// Send a Startup IPI (SIPI): @p vector is the trampoline page number
    /// (e.g. 0x08 -> AP starts real-mode execution at physical 0x8000).
    void send_sipi(uint8_t dest_apic_id, uint8_t vector);

private:
    volatile uint32_t* base_ = nullptr;
};

/// Global BSP Local APIC instance.
extern LocalAPIC g_lapic;

}  // namespace cinux::drivers::apic
