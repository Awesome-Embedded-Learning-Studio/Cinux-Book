/**
 * @file    acpi.hpp
 * @brief   The smallest slice of ACPI: find the tables, cut the power.
 *
 * Powering off a PC goes through ACPI: the firmware publishes a table
 * tree (RSDP at a known-ish place, pointing at a table directory,
 * pointing at the FADT), and the FADT names the PM1a control register
 * where one 16-bit write with the sleep-enable bit ends the machine.
 * This header walks exactly that spine — the full tree, AML and all,
 * is a later station's meal. All table bytes travel little-endian, so
 * the layout structs are read in place after a checksum.
 *
 * @author  Charliechen114514
 * @date    2026-10-11
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_driver
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <stdint.h>

namespace cinux::driver::acpi {

/**
 * @brief   The root pointer's on-disk shape, 36 bytes for ACPI 1.0.
 * @note    revision 0 carries only the RSDT half; revision 2 and up add
 *          the length, the 64-bit XSDT address and the second checksum.
 * @since   0.1.0
 * @ingroup kernel_driver
 */
struct Rsdp {
    char     signature[8];       ///< "RSD PTR ", note the trailing space.
    uint8_t  checksum;           ///< Sum of the first 20 bytes is zero.
    char     oem_id[6];          ///< Firmware vendor tag.
    uint8_t  revision;           ///< 0 for ACPI 1.0, 2 or later since.
    uint32_t rsdt_address;       ///< 32-bit pointer to the RSDT.
    uint32_t length;             ///< Bytes of this structure, revision 2+.
    uint64_t xsdt_address;       ///< 64-bit pointer, revision 2+.
    uint8_t  extended_checksum;  ///< Covers the whole structure.
};

/**
 * @brief   The 36-byte header every ACPI table starts with.
 * @note    Signature and length are all the walk reads; the checksum
 *          covers the whole table and is checked before use.
 * @since   0.1.0
 * @ingroup kernel_driver
 */
struct TableHeader {
    char     signature[4];  ///< "RSDT", "XSDT", "FACP", ...
    uint32_t length;        ///< Whole-table byte count.
    uint8_t  revision;      ///< Table format version.
    uint8_t  checksum;      ///< Whole-table sum is zero.
    char     oem_id[6];     ///< Firmware vendor tag.
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
};

/**
 * @brief   The fields of the FADT this driver consumes.
 * @note    Fixed-address control registers sit before any of the
 *          extended blocks, so every FADT revision answers the same
 *          two words at the same offsets.
 * @since   0.1.0
 * @ingroup kernel_driver
 */
struct Fadt {
    TableHeader header;        ///< "FACP".
    uint8_t     reserved[28];  ///< Fields this slice never reads.
    uint32_t    pm1a_control;  ///< I/O port of the PM1a control block.
    uint32_t    pm1b_control;  ///< Often zero; B blocks are optional.
};

/**
 * @brief         Whether a byte range is an ACPI table image.
 * @param[in]     bytes   The table's first byte.
 * @param[in]     length  The table's own length field.
 * @return        True when the whole-table sum is zero.
 * @since         0.1.0
 * @ingroup       kernel_driver
 */
[[nodiscard]] bool TableSumsToZero(const void* bytes, unsigned long length);

/**
 * @brief         Scans memory for the root pointer and its directory.
 * @return        Pointer to the RSDP, or nullptr when absent.
 * @note          Two sweeps after the spec's own order: the EBDA's
 *                first kilobyte, then the 0xE0000..0xFFFFF firmware
 *                window, both on 16-byte steps for the 8-byte
 *                signature. Virtual addresses come from the direct
 *                map, so every hit is readable as-is.
 * @since         0.1.0
 * @ingroup       kernel_driver
 */
[[nodiscard]] const Rsdp* FindRootPointer();

/**
 * @brief         Walks the directory for the FADT.
 * @param[in]     root   A pointer FindRootPointer handed back.
 * @return        The FACP table, or nullptr when the tree has none.
 * @note          Revision 0 roots point at an RSDT of 32-bit entries;
 *                later roots at an XSDT of 64-bit ones. Every entry is
 *                checksum-verified before it is trusted.
 * @since         0.1.0
 * @ingroup       kernel_driver
 */
[[nodiscard]] const Fadt* FindFadt(const Rsdp* root);

/**
 * @brief         Cuts the power through the PM1a control register.
 * @param[in]     fadt   The table FindFadt handed back.
 * @return        None
 * @note          Sleep type zero with the enable bit set is QEMU's S5
 *                encoding; real firmware spells the type in the DSDT's
 *                \_S5 package, a later station's parse. The write is
 *                16-bit; a return simply means the word went out and
 *                the firmware did not map power off there.
 * @since         0.1.0
 * @ingroup       kernel_driver
 */
void PowerOff(const Fadt* fadt);

}  // namespace cinux::driver::acpi
