#include "kernel/driver/ahci/ahci.hpp"

#include <stdint.h>

#include <array>

#include "cinux/bit_ops/bitmask.hpp"
#include "cinux/memory.hpp"
#include "cinux/ptr.hpp"
#include "kernel/arch/x86_64/instructions.hpp"
#include "kernel/arch/x86_64/irq_guard.hpp"
#include "kernel/arch/x86_64/page.hpp"
#include "kernel/boot/print.hpp"
#include "kernel/driver/ahci/ahci_identify.hpp"
#include "kernel/driver/ahci/ahci_layout.hpp"
#include "kernel/driver/block/block_device_concept.hpp"
#include "kernel/driver/pci/pci.hpp"
#include "kernel/driver/pci/pci_layout.hpp"
#include "kernel/interrupt/irq.hpp"
#include "kernel/mm/layout.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/vmm.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/time/tick.hpp"

namespace cinux::driver {

namespace {

/// @brief Bytes in one drive sector.
constexpr unsigned long kSectorBytes = 512;

/// @brief Sectors one command moves at most: the DMA page holds four KiB.
constexpr uint16_t kMaxSectorsPerCommand = 8;

/// @brief Host control bit: AHCI mode, without which nothing decodes.
constexpr AhciReg kGhcAhciEnable = cinux::base::bit::MaskBit<uint32_t>(31);

/// @brief Host control bit: the one gate every port interrupt passes.
constexpr AhciReg kGhcInterruptEnable = cinux::base::bit::MaskBit<uint32_t>(1);

/// @brief Host control bit: full reset, self-clearing.
constexpr AhciReg kGhcReset = cinux::base::bit::MaskBit<uint32_t>(0);

/// @brief Port command bit: the command engine runs.
constexpr AhciReg kCmdStart = cinux::base::bit::MaskBit<uint32_t>(0);

/// @brief Port command bit: received FIS memory is accepted.
constexpr AhciReg kCmdFisReceiveEnable = cinux::base::bit::MaskBit<uint32_t>(4);

/// @brief Port command bit: a FIS receive is in flight, waits for clear.
constexpr AhciReg kCmdFisReceiveRunning = cinux::base::bit::MaskBit<uint32_t>(14);

/// @brief Port command bit: a command list walk is in flight, ditto.
constexpr AhciReg kCmdCommandListRunning = cinux::base::bit::MaskBit<uint32_t>(15);

/// @brief Task file bit: the status byte's error flag, which the spec seats in the high half.
constexpr AhciReg kTfdError = cinux::base::bit::MaskBit<uint32_t>(8);

/// @brief Task file bit: the status byte's data request, same half.
constexpr AhciReg kTfdDataRequest = cinux::base::bit::MaskBit<uint32_t>(11);

/// @brief Task file bit: the status byte's device fault, same half.
constexpr AhciReg kTfdDeviceFault = cinux::base::bit::MaskBit<uint32_t>(13);

/// @brief Task file bit: the status byte's busy flag, same half.
constexpr AhciReg kTfdBusy = cinux::base::bit::MaskBit<uint32_t>(15);

/// @brief The status flags a finished command must have cleared.
constexpr AhciReg kTfdNotClean{kTfdError | kTfdDataRequest | kTfdDeviceFault | kTfdBusy};

/// @brief The status flags that must clear before the engine may start; a
///        stale error flag alone does not block the start.
constexpr AhciReg kTfdNotIdle{kTfdDataRequest | kTfdBusy};

/// @brief Port interrupt: a device-to-host FIS landed.
constexpr AhciReg kIsDeviceToHost = cinux::base::bit::MaskBit<uint32_t>(0);

/// @brief Port interrupt: a PIO setup FIS landed — a PIO command's only completion event.
constexpr AhciReg kIsPioSetup = cinux::base::bit::MaskBit<uint32_t>(1);

/// @brief Port interrupt: host bus data error, fatal.
constexpr AhciReg kIsHostBusData = cinux::base::bit::MaskBit<uint32_t>(28);

/// @brief Port interrupt: host bus fatal error.
constexpr AhciReg kIsHostBusFatal = cinux::base::bit::MaskBit<uint32_t>(29);

/// @brief Port interrupt: the task file reported an error.
constexpr AhciReg kIsTaskFileError = cinux::base::bit::MaskBit<uint32_t>(30);

/// @brief Port interrupt: interface fatal error.
constexpr AhciReg kIsInterfaceFatal = cinux::base::bit::MaskBit<uint32_t>(31);

/// @brief The events this driver watches.
constexpr AhciReg kWatched{kIsDeviceToHost | kIsPioSetup | kIsHostBusData | kIsHostBusFatal |
                           kIsTaskFileError | kIsInterfaceFatal};

/// @brief The events that fail the request outright.
constexpr AhciReg kFatal{kIsHostBusData | kIsHostBusFatal | kIsTaskFileError | kIsInterfaceFatal};

/// @brief The one command slot this driver issues through.
constexpr AhciReg kSlotZero = cinux::base::bit::MaskBit<uint32_t>(0);

/// @brief Which bar carries the register window: ICH9-style hosts answer in bar five.
constexpr uint8_t kRegistersBar = 5;

/// @brief Phy detection field of the serial status register.
constexpr cinux::base::bit::BitRange kPhyDetect{.low = 0, .width = 4};

/// @brief The phy detection field's spelling of device-present-and-ready.
constexpr uint32_t kPhyDetectActive = 0x3;

/// @brief Reset budget in ticks: the spec gives the host one second to self-clear.
constexpr unsigned long long kResetBudgetTicks = 100;

/// @brief Engine-stop budget in ticks: the spec asks software to allow five hundred ms.
constexpr unsigned long long kStopBudgetTicks = 50;

void irq_thunk() {
    Ahci::self().on_interrupt();
}

/// @brief Whether the register's watched bits already read as wanted.
bool bit_reads(const volatile uint32_t* reg, AhciReg bits, bool expect_set) {
    return AhciReg{*reg}.has(bits) == expect_set;
}

/// @brief Waits on a spec-guaranteed transition, without a limit.
void wait_forever(const volatile uint32_t* reg, AhciReg bits, bool expect_set) {
    while (!bit_reads(reg, bits, expect_set)) {
        cinux::arch::CpuRelax();
    }
}

/// @brief One observation step: check with interrupts masked, else sleep to
///        the next event and let the caller look again on wake.
bool observe_once(const volatile uint32_t* reg, AhciReg bits, bool expect_set) {
    const unsigned long long kSnapshot = cinux::arch::SaveAndDisableIrq();
    if (bit_reads(reg, bits, expect_set)) {
        cinux::arch::RestoreIrq(kSnapshot);
        return true;
    }
    cinux::arch::EnableIrqAndHalt();
    return false;
}

/// @brief Reports a stuck transition once its budget passed; later calls stay quiet.
void report_stuck_once(bool& reported, unsigned long long deadline, unsigned long long budget_ticks,
                       const char* what) {
    if (reported || time::Tick::self().since_boot() <= deadline) {
        return;
    }
    print::Println("[kern] ahci %s stuck past %u ticks, still waiting", what,
                   static_cast<unsigned>(budget_ticks));
    reported = true;
}

/// @brief Waits for watched bits to clear, sleeping to timer events and
///        reporting once the budget passes. The wait itself never ends
///        early: the budget diagnoses, it does not cancel.
void wait_clear_observed(const volatile uint32_t* reg, AhciReg bits,
                         unsigned long long budget_ticks, const char* what) {
    const unsigned long long kDeadline = time::Tick::self().since_boot() + budget_ticks;
    bool                     reported  = false;
    while (!observe_once(reg, bits, false)) {
        report_stuck_once(reported, kDeadline, budget_ticks, what);
    }
}

}  // namespace

bool Ahci::init() {
    const PciDevice* device =
        PciBus::self().find_by_class(PciClassCode::kMassStorage, kPciSubclassAhci);
    if (device == nullptr) {
        print::Println("[kern] ahci absent");
        return false;
    }
    PciBus::self().enable_device(*device);
    const auto kRegisters = static_cast<unsigned long>(BarBase(device->bars, kRegistersBar));
    if (!mm::MapDeviceWindow(kRegisters, cinux::arch::page::kSize)) {
        print::Println("[kern] ahci register window refused to map");
        return false;
    }
    regs_ = cinux::base::PtrAt<HbaMem>(mm::IoremapVirt(kRegisters));

    // NOLINTNEXTLINE(clang-analyzer-core.FixedAddressDereference) ioremap alias folds to a constant
    regs_->global_host_control = kGhcAhciEnable.raw;
    regs_->global_host_control = (kGhcAhciEnable | kGhcReset).raw;
    wait_clear_observed(&regs_->global_host_control, kGhcReset, kResetBudgetTicks, "host reset");
    regs_->global_host_control = kGhcAhciEnable.raw;

    const AhciReg kImplemented{regs_->ports_implemented};
    bool          found = false;
    for (unsigned long index = 0; index < kPortCount; ++index) {
        const AhciReg kPresent{kImplemented & cinux::base::bit::MaskBit<uint32_t>(index)};
        const AhciReg kStatus{regs_->ports[index].sata_status};
        if (kPresent.raw != 0 && kStatus.extract(kPhyDetect) == kPhyDetectActive) {
            port_index_ = index;
            found       = true;
            break;
        }
    }
    if (!found) {
        print::Println("[kern] ahci has no port with a drive on it");
        return false;
    }
    volatile HbaPort& port = regs_->ports[port_index_];

    port.sata_error = ~0U;
    port.command    = (AhciReg{port.command} & ~kCmdStart).raw;
    wait_clear_observed(&port.command, kCmdCommandListRunning, kStopBudgetTicks,
                        "command engine stop");
    port.command = (AhciReg{port.command} & ~kCmdFisReceiveEnable).raw;
    wait_clear_observed(&port.command, kCmdFisReceiveRunning, kStopBudgetTicks,
                        "receive engine stop");

    command_page_ = mm::Pmm::self().allocate_page();
    fis_page_     = mm::Pmm::self().allocate_page();
    data_page_    = mm::Pmm::self().allocate_page();
    cinux::base::SetBytes(cinux::base::PtrAt<void>(mm::DirectMapVirt(command_page_.raw)), 0,
                          cinux::arch::page::kSize);
    port.command_list_base       = static_cast<uint32_t>(command_page_.raw);
    port.command_list_base_upper = 0;
    port.fis_base                = static_cast<uint32_t>(fis_page_.raw);
    port.fis_base_upper          = 0;

    port.command = (AhciReg{port.command} | kCmdFisReceiveEnable).raw;
    wait_forever(&port.command, kCmdFisReceiveRunning, true);

    const AhciReg kPhy{regs_->ports[port_index_].sata_status};
    const AhciReg kTask{port.task_file_data};
    if (kPhy.extract(kPhyDetect) != kPhyDetectActive || (kTask & kTfdNotIdle).raw != 0) {
        print::Println("[kern] ahci port not idle before start, ssts %X tfd %X", kPhy.raw,
                       kTask.raw);
        return false;
    }
    port.command = (AhciReg{port.command} | kCmdStart).raw;
    wait_forever(&port.command, kCmdCommandListRunning, true);

    line_                 = interrupt::IrqLine{.value = device->interrupt_line};
    port.interrupt_status = kWatched.raw;
    regs_->interrupt_status =
        (cinux::base::bit::MaskBit<uint32_t>(static_cast<unsigned char>(port_index_))).raw;
    interrupt::Irq::self().register_handler(line_, irq_thunk);
    interrupt::Irq::self().enable_line(line_);
    port.interrupt_enable      = kWatched.raw;
    regs_->global_host_control = (kGhcAhciEnable | kGhcInterruptEnable).raw;

    cinux::base::SetBytes(cinux::base::PtrAt<void>(mm::DirectMapVirt(data_page_.raw)), 0xA5,
                          kSectorBytes);
    if (!issue(kAtaIdentifyDevice, 0, 1, false) || !wait_for_completion()) {
        print::Println("[kern] ahci identify did not complete");
        return false;
    }
    const auto* const kSeenHeader =
        cinux::base::PtrAt<const CommandHeader>(mm::DirectMapVirt(command_page_.raw));
    if (kSeenHeader->prd_byte_count != kSectorBytes) {
        print::Println("[kern] ahci identify moved %u bytes, not %u", kSeenHeader->prd_byte_count,
                       static_cast<unsigned>(kSectorBytes));
        return false;
    }
    std::array<uint16_t, 256> words{};
    cinux::base::CopyBytes(words.data(),
                           cinux::base::PtrAt<const void>(mm::DirectMapVirt(data_page_.raw)),
                           kSectorBytes);
    if (!IdentifyAdmits(words)) {
        std::array<char, 41> model{};
        ModelFromIdentify(words, model);
        print::Println("[kern] ahci identify failed admission, model '%s' word0 %X word83 %X",
                       model.data(), words[0], words[83]);
        return false;
    }
    sector_count_ = SectorCeilingFromIdentify(words);
    ModelFromIdentify(words, model_);
    print::Println("[kern] ahci %s, %llu sectors", model_.data(),
                   static_cast<unsigned long long>(sector_count_));

    std::array<uint8_t, kSectorBytes> pattern{};
    std::array<uint8_t, kSectorBytes> readback{};
    for (unsigned long index = 0; index < pattern.size(); ++index) {
        pattern[index] = static_cast<uint8_t>(index);
    }
    const BlockSpan kOneBlock{.value = 1};
    if (!write_blocks(Lba{.value = 1}, kOneBlock, pattern.data())) {
        print::Println("[kern] ahci scratch write failed");
        return false;
    }
    if (!read_blocks(Lba{.value = 1}, kOneBlock, readback.data())) {
        print::Println("[kern] ahci scratch read failed");
        return false;
    }
    const bool kMatches = cinux::base::EqualBytes(pattern.data(), readback.data(), pattern.size());
    cinux::base::SetBytes(pattern.data(), 0, pattern.size());
    if (!write_blocks(Lba{.value = 1}, kOneBlock, pattern.data())) {
        print::Println("[kern] ahci scratch restore failed");
        return false;
    }
    if (!flush()) {
        print::Println("[kern] ahci flush failed");
        return false;
    }
    if (kMatches) {
        print::Println("[kern] ahci scratch write-readback matches");
        return true;
    }
    print::Println("[kern] ahci scratch readback differs: %X %X %X %X", readback[0], readback[1],
                   readback[2], readback[3]);
    return false;
}

uint64_t Ahci::block_count() const {
    return sector_count_;
}

unsigned long Ahci::block_size() const {
    return kSectorBytes;
}

bool Ahci::read_blocks(Lba first, BlockSpan span, uint8_t* destination) {
    if (regs_ == nullptr || !issue(kAtaReadDmaExt, first.value, span.value, false)) {
        return false;
    }
    if (!wait_for_completion()) {
        return false;
    }
    cinux::base::CopyBytes(destination,
                           cinux::base::PtrAt<const void>(mm::DirectMapVirt(data_page_.raw)),
                           static_cast<unsigned long>(span.value) * kSectorBytes);
    return true;
}

bool Ahci::write_blocks(Lba first, BlockSpan span, const uint8_t* source) {
    if (regs_ == nullptr || span.value > kMaxSectorsPerCommand) {
        return false;
    }
    cinux::base::CopyBytes(cinux::base::PtrAt<void>(mm::DirectMapVirt(data_page_.raw)), source,
                           static_cast<unsigned long>(span.value) * kSectorBytes);
    return issue(kAtaWriteDmaExt, first.value, span.value, true) && wait_for_completion();
}

bool Ahci::flush() {
    return regs_ != nullptr && issue(kAtaFlushCacheExt, 0, 0, false) && wait_for_completion();
}

void Ahci::on_interrupt() {
    if (regs_ == nullptr) {
        return;
    }
    volatile HbaPort& port = regs_->ports[port_index_];
    const AhciReg     kEvents{port.interrupt_status};
    if (!kEvents.has(kWatched)) {
        return;
    }
    const AhciReg kIssue{port.command_issue};
    const AhciReg kTask{port.task_file_data};
    if (kEvents.has(kFatal)) {
        completion_.finish(false);
    } else if (kEvents.has(kIsDeviceToHost | kIsPioSetup) && !kIssue.has(kSlotZero) &&
               (kTask & kTfdNotClean).raw == 0) {
        completion_.finish(true);
    }
    port.interrupt_status = (kEvents & kWatched).raw;
    regs_->interrupt_status =
        (cinux::base::bit::MaskBit<uint32_t>(static_cast<unsigned char>(port_index_))).raw;
    [[maybe_unused]] const AhciReg kEcho{regs_->interrupt_status};
}

bool Ahci::issue(uint32_t command, uint64_t lba, uint16_t count, bool write) {
    if (count > kMaxSectorsPerCommand) {
        return false;
    }
    const unsigned long kBytes = static_cast<unsigned long>(count) * kSectorBytes;
    auto* const kHeader = cinux::base::PtrAt<CommandHeader>(mm::DirectMapVirt(command_page_.raw));
    auto* const kTable  = cinux::base::PtrAt<CommandTable>(mm::DirectMapVirt(command_page_.raw) +
                                                           kCommandTableOffset);
    FillRegisterFis(kTable->fis, command, lba, count);
    kHeader->flags              = MakeHeaderFlags(write, kBytes == 0 ? 0 : 1);
    kHeader->prd_byte_count     = 0;
    kHeader->command_table_base = static_cast<uint32_t>(command_page_.raw + kCommandTableOffset);
    kHeader->command_table_base_upper = 0;
    if (kBytes != 0) {
        kTable->regions[0].data_base       = static_cast<uint32_t>(data_page_.raw);
        kTable->regions[0].data_base_upper = 0;
        kTable->regions[0].reserved        = 0;
        kTable->regions[0].flags           = MakeRegionFlags(kBytes);
    }

    completion_.rearm();
    volatile HbaPort& port = regs_->ports[port_index_];
    port.interrupt_status  = kWatched.raw;
    asm volatile("" ::: "memory");
    port.command_issue = kSlotZero.raw;
    return true;
}

bool Ahci::wait_for_completion() {
    if (proc::Scheduler::self().current() != nullptr) {
        return completion_.wait();
    }
    for (;;) {
        const unsigned long long kSnapshot = cinux::arch::SaveAndDisableIrq();
        if (completion_.finished()) {
            cinux::arch::RestoreIrq(kSnapshot);
            return completion_.success();
        }
        cinux::arch::EnableIrqAndHalt();
    }
}

}  // namespace cinux::driver
