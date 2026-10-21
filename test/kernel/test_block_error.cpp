#include <array>

#include "cinux/ptr.hpp"
#include "kernel/driver/ahci/ahci.hpp"
#include "kernel/driver/ahci/ahci_layout.hpp"
#include "kernel/driver/block/block_device_concept.hpp"
#include "kernel/driver/pci/pci.hpp"
#include "kernel/driver/pci/pci_layout.hpp"
#include "kernel/mm/layout.hpp"
#include "test_assert.hpp"
#include "test_case.hpp"

namespace {

void fatal_command_quarantines_port() {
    auto&                          drive = cinux::driver::Ahci::self();
    std::array<unsigned char, 512> bytes{};
    const cinux::driver::BlockSpan kOne{.value = 1};
    ASSERT_TRUE(drive.read_blocks(cinux::driver::Lba{.value = 0}, kOne, bytes.data()));
    ASSERT_TRUE(
        !drive.read_blocks(cinux::driver::Lba{.value = drive.block_count()}, kOne, bytes.data()));
    const auto* device = cinux::driver::PciBus::self().find_by_class(
        cinux::driver::PciClassCode::kMassStorage, cinux::driver::kPciSubclassAhci);
    ASSERT_TRUE(device != nullptr);
    const auto  kBase = cinux::driver::BarBase(device->bars, 5);
    const auto* registers =
        cinux::base::PtrAt<const volatile cinux::driver::HbaMem>(cinux::mm::IoremapVirt(kBase));
    ASSERT_EQ(registers->ports[0].task_file_data & 1U, 1U);
    ASSERT_EQ(registers->ports[0].command & 1U, 0U);
    ASSERT_EQ(registers->ports[0].interrupt_enable, 0U);
    ASSERT_TRUE(!drive.read_blocks(cinux::driver::Lba{.value = 0}, kOne, bytes.data()));
    ASSERT_TRUE(!drive.flush());
}

[[gnu::used, gnu::section(".test_cases")]] constinit const cinux::test::TestCase kFatalCommand{
    .name = "block: real ATA task-file error stops the port and rejects later commands",
    .body = fatal_command_quarantines_port,
};

}
