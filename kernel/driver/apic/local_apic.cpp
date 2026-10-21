#include "kernel/driver/apic/local_apic.hpp"

#include <stdint.h>

#include "cinux/bit_ops/bitmask.hpp"
#include "cinux/ptr.hpp"
#include "kernel/arch/x86_64/instructions.hpp"
#include "kernel/arch/x86_64/page.hpp"
#include "kernel/mm/layout.hpp"
#include "kernel/mm/vmm.hpp"

namespace cinux::driver {

namespace {

constexpr unsigned int        kApicBaseMsr     = 0x1B;
constexpr auto                kHardwareEnabled = base::bit::MaskBit<uint64_t>(11);
constexpr auto                kX2ApicEnabled   = base::bit::MaskBit<uint64_t>(10);
constexpr base::bit::BitRange kBaseAddress{.low = 12, .width = 24};
constexpr base::bit::BitRange kApicId{.low = 24, .width = 8};
constexpr auto                kSoftwareEnabled  = base::bit::MaskBit<uint32_t>(8);
constexpr auto                kMasked           = base::bit::MaskBit<uint32_t>(16);
constexpr unsigned int        kIdRegister       = 0x20;
constexpr unsigned int        kPriorityRegister = 0x80;
constexpr unsigned int        kEoiRegister      = 0xB0;
constexpr unsigned int        kSpuriousRegister = 0xF0;
constexpr unsigned int        kIcrLow           = 0x300;
constexpr unsigned int        kIcrHigh          = 0x310;
constexpr unsigned int        kTimerLvt         = 0x320;
constexpr unsigned int        kTimerInitial     = 0x380;
constexpr unsigned int        kTimerCurrent     = 0x390;
constexpr unsigned int        kTimerDivide      = 0x3E0;
constexpr uint32_t            kDivideSixteen    = 3;
constexpr auto                kPeriodic         = base::bit::MaskBit<uint32_t>(17);
constexpr auto                kPending          = base::bit::MaskBit<uint32_t>(12);
constexpr unsigned int        kLvtRegisters[]   = {0x320, 0x330, 0x340, 0x350, 0x360, 0x370};

}

bool LocalApic::init(uint64_t physical) {
    const base::bit::BitMask<uint64_t> kBase{arch::ReadMsr(kApicBaseMsr)};
    const uint64_t kMsrPhysical = kBase.extract(kBaseAddress) * arch::page::kSize;
    if (!kBase.has(kHardwareEnabled) || kBase.has(kX2ApicEnabled) || physical == 0 ||
        physical != kMsrPhysical || !mm::MapDeviceWindow(physical, arch::page::kSize)) {
        return false;
    }
    registers_ = base::PtrAt<volatile uint32_t>(mm::IoremapVirt(physical));
    return true;
}

uint32_t LocalApic::read(unsigned int offset) const {
    return registers_[offset / sizeof(uint32_t)];
}

void LocalApic::write(unsigned int offset, uint32_t value) {
    registers_[offset / sizeof(uint32_t)] = value;
}

uint8_t LocalApic::id() const {
    return static_cast<uint8_t>(base::bit::BitMask<uint32_t>{read(kIdRegister)}.extract(kApicId));
}

void LocalApic::enable(uint8_t vector) {
    for (const unsigned int kRegister : kLvtRegisters) {
        write(kRegister, read(kRegister) | kMasked.raw);
    }
    write(kPriorityRegister, 0);
    write(kSpuriousRegister, static_cast<uint32_t>(vector) | kSoftwareEnabled.raw);
}

void LocalApic::eoi() {
    write(kEoiRegister, 0);
}

void LocalApic::send(uint8_t destination, uint32_t command) {
    while (base::bit::BitMask<uint32_t>{read(kIcrLow)}.has(kPending)) {
        arch::CpuRelax();
    }
    base::bit::BitMask<uint32_t> high;
    high.deposit(kApicId, destination);
    write(kIcrHigh, high.raw);
    write(kIcrLow, command);
}

void LocalApic::start_timer(uint8_t vector, uint32_t count, bool periodic) {
    write(kTimerDivide, kDivideSixteen);
    write(kTimerLvt, static_cast<uint32_t>(vector) | (periodic ? kPeriodic.raw : kMasked.raw));
    write(kTimerInitial, count);
}

uint32_t LocalApic::timer_remaining() const {
    return read(kTimerCurrent);
}

void LocalApic::stop_timer() {
    write(kTimerLvt, kMasked.raw);
    write(kTimerInitial, 0);
}

}
