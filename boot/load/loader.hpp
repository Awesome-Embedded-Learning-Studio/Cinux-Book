/**
 * @file    loader.hpp
 * @brief   Kernel-image ferry contract: read, validate, and deliver.
 *
 * The disk side of the handoff: one window at the ferry address is all
 * real mode can move at a time, so every transfer is sector math plus a
 * ferry trip. The header scratch and validation verdicts live in
 * image_header.hpp; this file owns the disk-to-memory mechanics.
 *
 * @author  Charliechen114514
 * @date    2026-10-01
 * @version 0.1
 * @since   0.1.0
 * @ingroup boot_loader
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "kernel/boot/image_header.hpp"

namespace cinux::boot::load {

/** @brief Sectors one extended-read window can carry (ferry window minus slack). */
inline constexpr unsigned int kFerryWindowSectors = 127;

/**
 * @brief         Sectors needed to carry the given byte count, capped.
 *
 * @param[in]     bytes   Byte count to cover, rounding up.
 * @param[in]     cap     Sector ceiling a single window allows.
 * @return        The sector count to request, never above cap.
 * @note          None
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       boot_loader
 */
inline unsigned int SectorsFor(unsigned long long bytes, unsigned int cap) {
    auto const kNeeded = static_cast<unsigned int>((bytes + 511ULL) / 512ULL);
    return kNeeded < cap ? kNeeded : cap;
}

/**
 * @brief         Bytes occupied by a whole number of sectors.
 *
 * @param[in]     sectors   Sector count.
 * @return        sectors * 512 as the byte extent.
 * @note          None
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       boot_loader
 */
inline unsigned long SectorBytes(unsigned int sectors) {
    return static_cast<unsigned long>(sectors) * 512U;
}

/**
 * @brief         Reads the kernel image header from disk into the scratch.
 *
 * @return        The header as it lies on disk at the kernel image LBA.
 * @note          Loads the GDT first so the ferry round trip that follows
 *                already runs under the switch descriptors.
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       boot_loader
 */
ImageHeader ReadHeader();

/**
 * @brief         Ferries the whole kernel image from disk to memory.
 *
 * @param[in]     header   The validated image header.
 * @return        LoadStatus::kOk on success, otherwise the failure reason.
 * @note          The first window also copies the on-disk magic back for
 *                a delivery check after the ferry trip.
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       boot_loader
 */
LoadStatus LoadKernel(const ImageHeader& header);

/**
 * @brief         Copies memory through the 32-bit ferry.
 *
 * @param[in]     dst   Destination linear address.
 * @param[in]     src   Source linear address.
 * @param[in]     len   Byte count to move.
 * @return        None
 * @note          Enters protected mode briefly; callers must be in real
 *                mode with the GDT loaded.
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       boot_loader
 */
void RunFerry(unsigned dst, unsigned src, unsigned len);

}  // namespace cinux::boot::load
