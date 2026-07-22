/**
 * @file test/unit/test_msix.cpp
 * @brief Host unit tests for PCI MSI-X capability discovery
 *
 * Exercises the REAL cinux::drivers::pci::msix::find_capability() (linked
 * from kernel/drivers/pci/msix.cpp) against a mock config-space image
 * supplied through a ConfigReader.  No real 0xCF8/0xCFC I/O.
 *
 * Coverage:
 *   - MSI-X found mid-list: cap offset / message control / table size
 *   - Table size field encoding (entries = MC[10:0] + 1)
 *   - Table Offset + BIR decode
 *   - PBA Offset + distinct BIR decode
 *   - Offset/BIR bit split (low 3 bits are BIR, not offset)
 *   - Not found: capability list absent (STATUS bit clear)
 *   - Not found: list present but no MSI-X
 *   - Not found: empty cap pointer (0x34 == 0)
 *   - MSI cap (id 0x05) skipped, MSI-X after it still found
 *
 * Compile condition: -DCINUX_HOST_TEST
 */

#define TEST_FRAMEWORK_IMPL
#include "test_framework.h"

#ifdef CINUX_HOST_TEST

#    include <cstdint>
#    include <cstring>

#    include "drivers/pci/msix.hpp"
#    include "drivers/pci/pci_config.hpp"

// using-directive (not using-declaration): the PCI register namespaces
// (PciReg/PciCapId/PciStatus) are themselves namespaces, which older GCC
// rejects as using-declaration targets.
using namespace cinux::drivers::pci;
using cinux::drivers::pci::msix::MsixCap;
using cinux::drivers::pci::msix::MsixTableEntry;
using cinux::drivers::pci::msix::find_capability;
using cinux::drivers::pci::msix::message_control_unmask_function;
using cinux::drivers::pci::msix::message_control_with_enable;
using cinux::drivers::pci::msix::xapic_message_address;
using cinux::drivers::pci::msix::xapic_message_data;

// ============================================================
// Mock PCI config space (256 bytes = 64 dwords)
// ============================================================

static constexpr uint32_t kConfigDwords = 64;
static uint32_t           g_cfg[kConfigDwords];

static void cfg_reset() {
    std::memset(g_cfg, 0, sizeof(g_cfg));
}

// Write a capability header dword [id | next | msg_ctrl] at a dword offset.
static void cfg_put_cap(uint8_t offset, uint8_t id, uint8_t next, uint16_t msg_ctrl = 0) {
    g_cfg[offset >> 2] = static_cast<uint32_t>(id) | (static_cast<uint32_t>(next) << 8) |
                         (static_cast<uint32_t>(msg_ctrl) << 16);
}

// Mark STATUS (high word of dword at 0x04) with the capabilities-list bit.
static void cfg_enable_cap_list(uint8_t first_cap_offset) {
    g_cfg[PciReg::COMMAND >> 2]              = static_cast<uint32_t>(PciStatus::CAP_LIST) << 16;
    g_cfg[PciReg::CAPABILITIES_POINTER >> 2] = first_cap_offset;
}

// Config-space reader matching PCI::pci_read's signature.
static uint32_t mock_reader(uint8_t /*bus*/, uint8_t /*slot*/, uint8_t /*func*/, uint8_t offset) {
    return g_cfg[(offset >> 2) % kConfigDwords];
}

// ============================================================
// 1. MSI-X found mid-list
// ============================================================

/// Build [PM @0x70] -> [MSI-X @0x80(end)] with given msg_ctrl + Table/PBA dwords.
static void build_list_with_msix(uint16_t msg_ctrl, uint32_t table_dword, uint32_t pba_dword) {
    cfg_reset();
    cfg_enable_cap_list(0x70);
    cfg_put_cap(0x70, PciCapId::POWER_MANAGEMENT, 0x80);
    cfg_put_cap(0x80, PciCapId::MSI_X, 0x00, msg_ctrl);
    g_cfg[(0x80 + 4) >> 2] = table_dword;
    g_cfg[(0x80 + 8) >> 2] = pba_dword;
}

TEST("msix: finds MSI-X mid-list with correct cap offset") {
    build_list_with_msix(0x0003, 0x00002000, 0x00003000);
    MsixCap cap = find_capability(0, 0, 0, &mock_reader);
    ASSERT_TRUE(cap.found);
    ASSERT_EQ(cap.cap_offset, 0x80);
}

TEST("msix: decodes message control and table size") {
    // MC[10:0] = 3 -> table size 4.
    build_list_with_msix(0x0003, 0x00002000, 0x00003000);
    MsixCap cap = find_capability(0, 0, 0, &mock_reader);
    ASSERT_TRUE(cap.found);
    ASSERT_EQ(cap.message_control, 0x0003);
    ASSERT_EQ(cap.table_size, 4u);
}

TEST("msix: table size field encoding (entries = field + 1)") {
    // MC[10:0] = 0x1F -> 32 entries.
    build_list_with_msix(0x001F, 0x00002000, 0x00003000);
    MsixCap cap = find_capability(0, 0, 0, &mock_reader);
    ASSERT_TRUE(cap.found);
    ASSERT_EQ(cap.table_size, 32u);
}

TEST("msix: decodes table offset and BIR") {
    build_list_with_msix(0x0003, 0x00002000, 0x00003000);
    MsixCap cap = find_capability(0, 0, 0, &mock_reader);
    ASSERT_EQ(cap.table_offset, 0x2000u);
    ASSERT_EQ(cap.table_bar, 0u);
}

TEST("msix: decodes pba offset and a distinct BIR") {
    // Table in BAR0, PBA in BAR1 (BIR=1).
    build_list_with_msix(0x0003, 0x00002000, 0x00003001);
    MsixCap cap = find_capability(0, 0, 0, &mock_reader);
    ASSERT_EQ(cap.pba_offset, 0x3000u);
    ASSERT_EQ(cap.pba_bar, 1u);
}

TEST("msix: low 3 bits of the dword are BIR, not offset") {
    // 0x00002005 -> offset 0x2000, BIR 5.
    build_list_with_msix(0x0003, 0x00002005, 0x00003000);
    MsixCap cap = find_capability(0, 0, 0, &mock_reader);
    ASSERT_EQ(cap.table_offset, 0x2000u);
    ASSERT_EQ(cap.table_bar, 5u);
}

// ============================================================
// 2. Not-found cases
// ============================================================

TEST("msix: not found when capability list absent") {
    cfg_reset();  // STATUS cap-list bit clear
    MsixCap cap = find_capability(0, 0, 0, &mock_reader);
    ASSERT_FALSE(cap.found);
}

TEST("msix: not found when list has no MSI-X") {
    cfg_reset();
    cfg_enable_cap_list(0x70);
    cfg_put_cap(0x70, PciCapId::POWER_MANAGEMENT, 0x00);  // end of list
    MsixCap cap = find_capability(0, 0, 0, &mock_reader);
    ASSERT_FALSE(cap.found);
}

TEST("msix: not found when cap pointer is 0") {
    cfg_reset();
    cfg_enable_cap_list(0x00);  // empty list
    MsixCap cap = find_capability(0, 0, 0, &mock_reader);
    ASSERT_FALSE(cap.found);
}

// ============================================================
// 3. MSI (id 0x05) is skipped, MSI-X after it still found
// ============================================================

TEST("msix: skips MSI capability, finds MSI-X after it") {
    cfg_reset();
    cfg_enable_cap_list(0x60);
    cfg_put_cap(0x60, PciCapId::MSI, 0x70);  // MSI, skipped
    cfg_put_cap(0x70, PciCapId::MSI_X, 0x00, 0x0007);
    g_cfg[(0x70 + 4) >> 2] = 0x00001000;
    g_cfg[(0x70 + 8) >> 2] = 0x00001800;
    MsixCap cap            = find_capability(0, 0, 0, &mock_reader);
    ASSERT_TRUE(cap.found);
    ASSERT_EQ(cap.cap_offset, 0x70);
    ASSERT_EQ(cap.table_size, 8u);
}

// ============================================================
// 4. Pure helpers (Batch 0B): xAPIC message format + Message Control bits
// ============================================================

TEST("msix: MsixTableEntry is 16 bytes") {
    ASSERT_EQ(sizeof(MsixTableEntry), 16u);
}

TEST("msix: xapic message address for BSP (dest 0)") {
    ASSERT_EQ(xapic_message_address(0), 0xFEE00000u);
}

TEST("msix: xapic message address encodes dest in bits 19:12") {
    ASSERT_EQ(xapic_message_address(5), 0xFEE05000u);
}

TEST("msix: xapic message data is the vector (fixed/edge/phys)") {
    ASSERT_EQ(xapic_message_data(0x40), 0x40u);
}

TEST("msix: message_control_with_enable sets bit 15") {
    ASSERT_EQ(message_control_with_enable(0x0003), 0x8003u);
}

TEST("msix: message_control_unmask_function clears bit 14") {
    ASSERT_EQ(message_control_unmask_function(0x4003), 0x0003u);
}

int main() {
    RUN_ALL_TESTS();
    return _tests_failed > 0 ? 1 : 0;
}

#endif  // CINUX_HOST_TEST
