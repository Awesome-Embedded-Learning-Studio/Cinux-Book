/**
 * @file    ahci_layout.hpp
 * @brief   AHCI wire types and spec constants, with encoders in one TU.
 *
 * The register block is byte-aligned, so it is plain structs with
 * static_asserted sizes and offsets; flag bits go through the BitMask
 * vocabulary where the encoders pack them, in ahci_layout.cpp — one TU
 * both worlds compile, so host tests check the packing without a
 * controller in sight. Identify-data parsing lives in ahci_identify;
 * the cycles live in ahci.cpp.
 *
 * @author  Charliechen114514
 * @date    2026-10-09
 * @version 0.2
 * @since   0.1.0
 * @ingroup kernel_driver
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <stdint.h>

#include <array>
#include <cstddef>

#include "cinux/bit_ops/bitmask.hpp"

namespace cinux::driver {

/// @brief One MMIO register as the bit vocabulary sees it.
using AhciReg = cinux::base::bit::BitMask<uint32_t>;

/// @brief Ports one host controller may carry.
inline constexpr unsigned long kPortCount = 32;

/// @brief Offset of the one command table inside the command page.
inline constexpr unsigned long kCommandTableOffset = 0x400;

/// @brief ATA opcode: report the drive's identity words.
inline constexpr uint8_t kAtaIdentifyDevice = 0xEC;

/// @brief ATA opcode: read DMA with 48-bit addressing.
inline constexpr uint8_t kAtaReadDmaExt = 0x25;

/// @brief ATA opcode: write DMA with 48-bit addressing.
inline constexpr uint8_t kAtaWriteDmaExt = 0x35;

/// @brief ATA opcode: flush the drive's write cache to the medium.
inline constexpr uint8_t kAtaFlushCacheExt = 0xEA;

/// @brief FIS type: a register request from host to device.
inline constexpr uint8_t kFisRegisterHostToDevice = 0x27;

/// @brief FIS flag: the device shall update its command register.
inline constexpr uint8_t kFisFlagCommand = 0x80;

/// @brief Device register value asking for 48-bit LBA addressing.
inline constexpr uint8_t kAtaDeviceLba = 0x40;

static_assert(kFisRegisterHostToDevice == 0x27, "register FIS type is 0x27 per the spec table");
static_assert(kFisFlagCommand == 0x80,
              "the command flag is bit seven, not the scouting note's six");
static_assert(kAtaDeviceLba == 0x40, "the LBA selector is 0x40");

/**
 * @brief   One port's register block, 0x80 bytes of the host's memory.
 * @since   0.1.0
 * @ingroup kernel_driver
 */
struct HbaPort {
    uint32_t                command_list_base;        ///< 0x00: physical base of the 32 headers.
    uint32_t                command_list_base_upper;  ///< 0x04: high half above four GiB.
    uint32_t                fis_base;           ///< 0x08: physical base of the received-FIS area.
    uint32_t                fis_base_upper;     ///< 0x0C: high half above four GiB.
    uint32_t                interrupt_status;   ///< 0x10: what happened; write one to clear.
    uint32_t                interrupt_enable;   ///< 0x14: which happenings may raise the line.
    uint32_t                command;            ///< 0x18: the engine's start and receive switches.
    uint32_t                reserved0;          ///< 0x1C.
    uint32_t                task_file_data;     ///< 0x20: the drive's status and error byte.
    uint32_t                signature;          ///< 0x24: what kind of device answered.
    uint32_t                sata_status;        ///< 0x28: phy state, detection among it.
    uint32_t                sata_control;       ///< 0x2C.
    uint32_t                sata_error;         ///< 0x30.
    uint32_t                sata_active;        ///< 0x34.
    uint32_t                command_issue;      ///< 0x38: write a slot bit to launch it.
    uint32_t                sata_notification;  ///< 0x3C.
    uint32_t                fis_based_switching;  ///< 0x40.
    std::array<uint32_t, 9> reserved1;            ///< 0x44..0x67.
    std::array<uint32_t, 2> vendor;               ///< 0x68..0x6F.
    std::array<uint32_t, 4> reserved2;            ///< 0x70..0x7F.
};

static_assert(sizeof(HbaPort) == 0x80, "port register block is 0x80 bytes");
static_assert(offsetof(HbaPort, interrupt_status) == 0x10, "PxIS seats at 0x10");
static_assert(offsetof(HbaPort, command) == 0x18, "PxCMD seats at 0x18");
static_assert(offsetof(HbaPort, task_file_data) == 0x20, "PxTFD seats at 0x20");
static_assert(offsetof(HbaPort, command_issue) == 0x38, "PxCI seats at 0x38");

/**
 * @brief   The host controller's register block: globals then 32 ports.
 * @note    The port array stays native: the driver reaches it through a
 *          volatile window, and std::array answers no volatile index.
 * @since   0.1.0
 * @ingroup kernel_driver
 */
struct HbaMem {
    uint32_t capabilities;                       ///< 0x00: what the controller can do.
    uint32_t global_host_control;                ///< 0x04: reset, gate, AHCI switch.
    uint32_t interrupt_status;                   ///< 0x08: which ports raised, one bit each.
    uint32_t ports_implemented;                  ///< 0x0C: which ports exist, one bit each.
    uint32_t version;                            ///< 0x10: the spec revision it speaks.
    uint32_t ccc_control;                        ///< 0x14: coalescing timer.
    uint32_t ccc_ports;                          ///< 0x18: coalescing ports bitmap.
    uint32_t enclosure_location;                 ///< 0x1C.
    uint32_t enclosure_control;                  ///< 0x20.
    uint32_t capabilities2;                      ///< 0x24.
    uint32_t bios_handoff;                       ///< 0x28: ownership dance with firmware.
    std::array<uint8_t, 0x100 - 0x2C> reserved;  ///< 0x2C..0xFF.
    HbaPort                           ports[kPortCount];  ///< 0x100: every port, present or not.
};

static_assert(sizeof(HbaMem) == 0x100 + (kPortCount * 0x80), "ports begin at 0x100");
static_assert(offsetof(HbaMem, ports) == 0x100, "the port array seats at 0x100");

/**
 * @brief   One command header: where its FIS lives and how long it is.
 * @since   0.1.0
 * @ingroup kernel_driver
 */
struct CommandHeader {
    uint32_t                flags;  ///< DW0: fis length and direction below, leg count above.
    uint32_t                prd_byte_count;      ///< DW1: bytes moved, the completion report.
    uint32_t                command_table_base;  ///< DW2: physical base of this command's table.
    uint32_t                command_table_base_upper;  ///< DW3: high half above four GiB.
    std::array<uint32_t, 4> reserved;
};

static_assert(offsetof(CommandHeader, prd_byte_count) == 0x04,
              "the byte count is dword one, not the leg count");
static_assert(offsetof(CommandHeader, command_table_base) == 0x08, "the table base is dword two");
static_assert(sizeof(CommandHeader) == 32, "command headers are 32 bytes");

/**
 * @brief   One scatter-gather leg: a physical span the DMA engine walks.
 * @since   0.1.0
 * @ingroup kernel_driver
 */
struct PhysicalRegion {
    uint32_t data_base;        ///< DW0: physical address of the span's start.
    uint32_t data_base_upper;  ///< DW1: high half when above four GiB.
    uint32_t reserved;         ///< DW2: zero per the spec.
    uint32_t flags;            ///< DW3: byte count minus one, and the completion bit.
};

static_assert(sizeof(PhysicalRegion) == 16, "region descriptors are 16 bytes");
static_assert(offsetof(PhysicalRegion, flags) == 0x0C,
              "the byte count rides the last dword, not the third");

/**
 * @brief   The register host-to-device FIS as the spec tables it.
 * @note    The 48-bit sector address is split around a device byte, so
 *          the interleave lives in the field order where the spec put
 *          it, not in the code that fills it.
 * @since   0.1.0
 * @ingroup kernel_driver
 */
struct RegisterFis {
    uint8_t                 type;           ///< 0x00: the host-to-device type.
    uint8_t                 flags;          ///< 0x01: the command bit lives here.
    uint8_t                 command;        ///< 0x02: ATA opcode.
    uint8_t                 features_low;   ///< 0x03: feature bits, zero for plain DMA.
    std::array<uint8_t, 3>  lba_low;        ///< 0x04..0x06: sector address, low half.
    uint8_t                 device;         ///< 0x07: the LBA selector lives here.
    std::array<uint8_t, 3>  lba_high;       ///< 0x08..0x0A: sector address, high half.
    uint8_t                 features_high;  ///< 0x0B: feature bits' high half, zero ditto.
    uint16_t                count;          ///< 0x0C..0x0D: sectors to move, native endian.
    uint8_t                 icc;            ///< 0x0E.
    uint8_t                 control;        ///< 0x0F.
    std::array<uint8_t, 48> zero;           ///< 0x10..0x3F: the FIS is one fixed size.
};

static_assert(sizeof(RegisterFis) == 64, "register FIS is 64 bytes");
static_assert(offsetof(RegisterFis, features_low) == 0x03,
              "byte three carries features, not the device register");
static_assert(offsetof(RegisterFis, device) == 0x07,
              "the device register rides byte seven, mid-address");

/**
 * @brief   One command's table: the FIS, then atapi bytes, then regions.
 * @since   0.1.0
 * @ingroup kernel_driver
 */
struct CommandTable {
    RegisterFis                   fis;    ///< The request the drive reads.
    std::array<uint8_t, 16>       atapi;  ///< Packet bytes, unused for reg requests.
    std::array<uint8_t, 48>       reserved;
    std::array<PhysicalRegion, 1> regions;  ///< The one leg this driver lays out.
};

static_assert(sizeof(CommandTable) == 0x90, "the command table is 144 bytes");
static_assert(offsetof(CommandTable, regions) == 0x80,
              "the region descriptors seat at 0x80, after fis and atapi and padding");
static_assert(kCommandTableOffset == 32 * sizeof(CommandHeader),
              "the table follows all thirty-two command headers");

/**
 * @brief         Fills one register host-to-device FIS for a DMA command.
 *
 * @param[out]    fis     The FIS to overwrite, zeroed first.
 * @param[in]     command ATA opcode, one of the kAta constants.
 * @param[in]     lba     First sector, 48-bit.
 * @param[in]     count   Sectors to move.
 * @return        None
 * @note          The command flag is bit seven of the flags byte; the
 *                      scouting note that placed it at bit six left the
 *                      drive silent, and bit six alone commands nothing.
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       kernel_driver
 */
void FillRegisterFis(RegisterFis& fis, uint32_t command, uint64_t lba, uint16_t count);

/**
 * @brief         Packs one command header's flag word.
 *
 * @param[in]     write   True when the command carries data to the drive.
 * @param[in]     legs    Scatter-gather legs the command's table carries.
 * @return        The header flag dword.
 * @note          FIS length is fixed: a register FIS is five dwords.
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       kernel_driver
 */
[[nodiscard]] uint32_t MakeHeaderFlags(bool write, unsigned long legs);

/**
 * @brief         Packs one region's flag word.
 *
 * @param[in]     bytes   Size of the span, must be a nonzero multiple of
 *                      the sector size.
 * @return        The region flag dword with the completion bit set.
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       kernel_driver
 */
[[nodiscard]] uint32_t MakeRegionFlags(unsigned long bytes);

}  // namespace cinux::driver
