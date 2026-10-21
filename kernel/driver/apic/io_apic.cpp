#include "kernel/driver/apic/io_apic.hpp"

#include <stdint.h>

#include "cinux/assert.hpp"
#include "cinux/bit_ops/bitmask.hpp"
#include "cinux/ptr.hpp"
#include "kernel/arch/x86_64/page.hpp"
#include "kernel/arch/x86_64/vector_config.hpp"
#include "kernel/driver/acpi/madt.hpp"
#include "kernel/driver/apic/local_apic.hpp"
#include "kernel/interrupt/irq.hpp"
#include "kernel/interrupt/irq_config.hpp"
#include "kernel/mm/layout.hpp"
#include "kernel/mm/vmm.hpp"
#include "kernel/proc/sync.hpp"

namespace cinux::driver {

namespace {

constexpr unsigned int kWindow          = 0x10 / sizeof(uint32_t);
constexpr unsigned int kVersionRegister = 1;
constexpr unsigned int kRedirectBase    = 0x10;
constexpr unsigned int kWordsPerRoute   = 2;
using arch::irq::kFirstIrqVector;
proc::Spinlock                g_register_lock;
constexpr base::bit::BitRange kLastPin{.low = 16, .width = 8};
constexpr base::bit::BitRange kDestination{.low = 24, .width = 8};
constexpr base::bit::BitRange kPolarity{.low = 0, .width = 2};
constexpr base::bit::BitRange kTrigger{.low = 2, .width = 2};
constexpr auto                kMasked             = base::bit::MaskBit<uint32_t>(16);
constexpr auto                kActiveLow          = base::bit::MaskBit<uint32_t>(13);
constexpr auto                kLevelTriggered     = base::bit::MaskBit<uint32_t>(15);
constexpr uint32_t            kExplicitLowOrLevel = 3;

}

uint32_t IoApic::read(unsigned int index) {
    registers_[0] = index;
    return registers_[kWindow];
}

void IoApic::write(unsigned int index, uint32_t value) {
    registers_[0]       = index;
    registers_[kWindow] = value;
}

bool IoApic::init(const acpi::InterruptTopology& topology) {
    if (topology.io_apic_address == 0 || topology.io_apic_address % arch::page::kSize != 0 ||
        !mm::MapDeviceWindow(topology.io_apic_address, arch::page::kSize)) {
        return false;
    }
    registers_   = base::PtrAt<volatile uint32_t>(mm::IoremapVirt(topology.io_apic_address));
    gsi_base_    = topology.io_apic_gsi_base;
    pin_count_   = base::bit::BitMask<uint32_t>{read(kVersionRegister)}.extract(kLastPin) + 1;
    destination_ = LocalApic::self().id();
    for (unsigned int pin = 0; pin < pin_count_; ++pin) {
        write(kRedirectBase + (pin * kWordsPerRoute), kMasked.raw);
    }
    for (unsigned int source = 0; source < interrupt::kIrqLineCount; ++source) {
        routes_[source] = {.source = static_cast<uint8_t>(source), .gsi = source, .flags = 0};
    }
    for (unsigned int index = 0; index < topology.override_count; ++index) {
        const auto& route     = topology.overrides[index];
        routes_[route.source] = route;
    }
    for (const auto& route : routes_) {
        const base::bit::BitMask<uint32_t> kFlags{route.flags};
        if (route.gsi < gsi_base_ || route.gsi - gsi_base_ >= pin_count_ ||
            kFlags.extract(kPolarity) == 2 || kFlags.extract(kTrigger) == 2) {
            return false;
        }
    }
    return true;
}

void IoApic::unmask(interrupt::IrqLine line) {
    base::safety::Check(line.value < interrupt::kIrqLineCount, "invalid ISA interrupt line");
    const auto                         kGuard = g_register_lock.guard();
    const auto&                        route  = routes_[line.value];
    const base::bit::BitMask<uint32_t> kFlags{route.flags};
    uint32_t                           low = kFirstIrqVector + line.value;
    if (kFlags.extract(kPolarity) == kExplicitLowOrLevel) {
        low |= kActiveLow.raw;
    }
    if (kFlags.extract(kTrigger) == kExplicitLowOrLevel) {
        low |= kLevelTriggered.raw;
    }
    base::bit::BitMask<uint32_t> high;
    high.deposit(kDestination, destination_);
    const unsigned int kRegister = kRedirectBase + ((route.gsi - gsi_base_) * kWordsPerRoute);
    write(kRegister, low | kMasked.raw);
    write(kRegister + 1, high.raw);
    write(kRegister, low);
}

void IoApic::ack([[maybe_unused]] interrupt::IrqLine line) {
    LocalApic::self().eoi();
}

}
