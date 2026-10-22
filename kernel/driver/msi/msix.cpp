/**
 * @file    msix.cpp
 * @brief   The MSI-X write side: protocol over one table entry.
 *
 * Field positions are the only thing hand-written; nothing here touches
 * a port or names a device. One TU both worlds compile, so the host
 * tests run the protocol over plain memory.
 *
 * @author  Charliechen114514
 * @date    2026-10-22
 * @version 0.1
 * @since   0.6.0
 * @ingroup kernel_driver
 * @copyright Copyright (c) 2026
 */

#include "kernel/driver/msi/msix.hpp"

#include <stdint.h>

namespace cinux::driver::msi {

namespace {

/// @brief The local controller's message window, the xAPIC convention.
constexpr uint64_t kMessageWindow = 0xFEE00000ULL;

/// @brief Where the destination id sits inside a message address.
constexpr unsigned kDestinationShift = 12;

/// @brief The entry control bit that keeps one entry from firing.
constexpr uint32_t kEntryMasked = 1U;

}  // namespace

uint64_t XapicMessageAddress(uint8_t destination) {
    return kMessageWindow | (static_cast<uint64_t>(destination) << kDestinationShift);
}

bool EntryLatched(const volatile MsixEntry& entry, uint64_t address, uint32_t data) {
    return entry.address == address && entry.data == data;
}

bool ArmMsixEntry(volatile MsixEntry& entry, uint64_t address, uint32_t data) {
    entry.control = kEntryMasked;
    entry.address = address;
    entry.data    = data;
    if (!EntryLatched(entry, address, data)) {
        return false;
    }
    entry.control = 0;
    return true;
}

}  // namespace cinux::driver::msi
