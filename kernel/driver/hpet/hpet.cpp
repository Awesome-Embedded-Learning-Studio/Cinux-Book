#include "kernel/driver/hpet/hpet.hpp"

#include <cstdint>

#include "kernel/driver/acpi/acpi.hpp"
#include "kernel/driver/hpet/hpet_layout.hpp"
#include "kernel/mm/layout.hpp"
#include "kernel/mm/vmm.hpp"

namespace cinux::driver {

namespace {

/// Where the up-counter sits inside a timer block.
constexpr unsigned long kCounterOffset = 0xF0;

constexpr unsigned long kPeriodOffset = 0x04;
constexpr unsigned long kConfigOffset = 0x10;
constexpr uint32_t      kCounter64    = 1U << 13U;
constexpr uint32_t      kEnable       = 1U;
constexpr uint32_t      kLegacyRoute  = 1U << 1U;

}  // namespace

bool Hpet::init(const acpi::Rsdp* root) {
    counter_                  = nullptr;
    period_fs_                = 0;
    boot_count_               = 0;
    const auto* const kHeader = acpi::FindTable(root, "HPET");
    if (kHeader == nullptr) {
        return false;
    }
    const uint64_t kBlock =
        hpet::BlockAddress(reinterpret_cast<const unsigned char*>(kHeader), kHeader->length);
    if (kBlock == 0 || kBlock > mm::kPml4EntrySpan - hpet::kBlockBytes ||
        !mm::MapDeviceWindow(kBlock, hpet::kBlockBytes)) {
        return false;
    }
    const auto* const kCaps =
        reinterpret_cast<const volatile uint32_t*>(  // NOLINT(performance-no-int-to-ptr)
            mm::IoremapVirt(kBlock));
    const auto* const kPeriod =
        reinterpret_cast<const volatile uint32_t*>(  // NOLINT(performance-no-int-to-ptr)
            mm::IoremapVirt(kBlock + kPeriodOffset));
    const uint64_t kPeriodFs = *kPeriod;
    if (!hpet::ValidPeriod(kPeriodFs) || (*kCaps & kCounter64) == 0) {
        return false;
    }
    auto* const kConfig =
        reinterpret_cast<volatile uint32_t*>(  // NOLINT(performance-no-int-to-ptr)
            mm::IoremapVirt(kBlock + kConfigOffset));
    // Start the free-running counter without taking IRQ0/IRQ8 away from PIT/RTC.
    // Comparator interrupt enables remain untouched; this driver never arms one.
    *kConfig = (*kConfig & ~kLegacyRoute) | kEnable;
    if ((*kConfig & kEnable) == 0) {
        return false;
    }
    period_fs_  = kPeriodFs;
    counter_    = reinterpret_cast<volatile uint64_t*>(  // NOLINT(performance-no-int-to-ptr)
        mm::IoremapVirt(kBlock + kCounterOffset));
    boot_count_ = *counter_;
    return true;
}

unsigned long long Hpet::since_boot_ns() const {
    if (!present()) {
        return 0;
    }
    const unsigned long long kTicks = *counter_ - boot_count_;
    return hpet::Nanoseconds(kTicks, period_fs_);
}

}  // namespace cinux::driver
