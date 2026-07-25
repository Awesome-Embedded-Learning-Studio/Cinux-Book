/**
 * @file kernel/drivers/hpet/hpet.hpp
 * @brief HPET (High-Precision Event Timer) free-running counter driver (F5-M4)
 *
 * Maps the HPET register block reported by the ACPI "HPET" table into the
 * KMEM_MMIO window (uncached, FLAG_PCD), enables the main counter, and exposes
 * a boot-relative monotonic time in nanoseconds.  This milestone reads only the
 * free-running counter -- the periodic-comparator / IRQ path (timer channels
 * routed to an IOAPIC vector, the LAPIC-timer analogue) is deferred.
 *
 * The PIT is untouched: it keeps driving BSP preemption (PIT::irq0_handler ->
 * Scheduler::tick).  HPET is a second, higher-resolution time source consumed by
 * sys_clock_gettime(CLOCK_MONOTONIC), not a PIT replacement.
 *
 * Namespace: cinux::drivers
 */

#pragma once

#include <stdint.h>

namespace cinux::drivers {

// ============================================================
// HPET MMIO register offsets (within the 4 KB window)
// ============================================================
// All accesses are 32-bit: QEMU (and some real HW) drops 64-bit MMIO writes
// even though 64-bit reads happen to work (QEMU splits them), so the portable,
// Linux-style access width is 32-bit and the 64-bit counter is read as two
// halves.
constexpr uint32_t kHpetRegGeneralCaps = 0x000;  ///< [31:0]=vendor/rev, [63:32]=period(fs)
constexpr uint32_t kHpetRegGeneralConfig =
    0x010;  ///< bit 0 = ENABLE_CNF (general regs are 0x10 apart)
constexpr uint32_t kHpetRegMainCounter = 0x0F0;  ///< 64-bit free-running counter

/// General Configuration bit 0: set to make the main counter increment.
constexpr uint32_t kHpetEnableCnf = 1u;

/// HPET-spec upper bound on the counter period (100 ns = 1e8 fs).
constexpr uint64_t kHpetMaxPeriodFs = 100'000'000ULL;

/// Convert HPET counter ticks to nanoseconds, overflow-safe.
///
/// The counter period is an integer number of femtoseconds, and a nanosecond is
/// 1e6 fs, so ns = ticks * period / 1e6 exactly.  The naive product overflows
/// uint64 for long uptimes (a year at 100 MHz is ~3e15 ticks * 1e7 = 3e22), so
/// the computation is split: ticks are decomposed into 1e6-blocks (hi) and a
/// remainder (lo), giving hi*period + (lo*period)/1e6, whose intermediates stay
/// well inside uint64 for centuries.
///
/// @param period_fs  Counter clock period in femtoseconds (General Capabilities).
/// @param ticks      Elapsed counter ticks.
/// @return           Equivalent nanoseconds.
inline uint64_t ticks_to_ns(uint64_t period_fs, uint64_t ticks) {
    constexpr uint64_t kDivisor = 1'000'000ULL;
    return (ticks / kDivisor) * period_fs + ((ticks % kDivisor) * period_fs) / kDivisor;
}

// ============================================================
// HPET driver
// ============================================================

/**
 * @brief HPET free-running counter driver (single instance).
 *
 * init() discovers the HPET via ACPI, maps its MMIO window (FLAG_PCD), records
 * the counter clock period, sets ENABLE_CNF so the main counter runs, and
 * captures the counter value as the boot baseline.  monotonic_ns() then reports
 * elapsed time since that baseline.  All methods are safe to call before
 * init() (they return 0 / false) so callers need no special guard.
 */
class HPET {
public:
    /// Discover + map + enable the HPET.  Idempotent.  @return true if available.
    bool init();

    /// Whether init() successfully enabled the counter.
    bool available() const { return base_ != nullptr; }

    /// Counter clock period in femtoseconds (0 if not initialised).
    uint64_t period_fs() const { return period_fs_; }

    /// Raw 64-bit main counter value (0 if not initialised).
    uint64_t counter() const;

    /// Boot-relative monotonic time in nanoseconds (0 if not initialised).
    uint64_t monotonic_ns() const;

private:
    uint32_t read32(uint32_t off) const;
    void     write32(uint32_t off, uint32_t value);
    uint64_t read_counter() const;  ///< 64-bit main counter via two 32-bit reads

    volatile uint32_t* base_         = nullptr;  ///< MMIO window (KMEM_MMIO+0x60000)
    uint64_t           period_fs_    = 0;        ///< counter tick period (femtoseconds)
    uint64_t           boot_counter_ = 0;        ///< counter value captured at init
};

/// Global HPET instance (BSP; the counter is shared across CPUs).
extern HPET g_hpet;

}  // namespace cinux::drivers
