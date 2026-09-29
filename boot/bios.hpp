/**
 * @file    bios.hpp
 * @brief   Real-mode BIOS services used by stage2.
 *
 * @author  Charliechen114514
 * @date    2026-09-27
 * @version 0.1
 * @since   0.1.0
 * @ingroup boot_bios
 * @copyright Copyright (c) 2026
 */

#pragma once
#include "e820/e820.hpp"
#include "vesa/vesa.hpp"

namespace cinux::boot::bios {

/**
 * @brief         Enables the A20 gate via INT 15h AX=2401.
 *
 * @return        true when the BIOS reports success (CF=0).
 * @note          Single BIOS method by design: emulator firmware always
 *                provides it; fallback chains would be over-engineering.
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       boot_bios
 */
bool EnableA20AddressLine();

/**
 * @brief         Collects the E820 memory map into map.
 *
 * @param[out]    map   Destination archive; entries are appended until the
 *                      BIOS continuation token reaches zero or the entry
 *                      cap is hit.
 * @return        0 on success, 1 when the BIOS reports an error.
 * @note          Detect-and-archive only; consumption belongs to later
 *                stations (PMM).
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       boot_bios
 */
int CollectMemoryMap(MemoryMap* map);

/**
 * @brief         Reads the VBE controller block via INT 10h AX=4F00.
 *
 * @param[out]    info   Controller block preloaded with the "VBE2" signature
 *                       by this call; the BIOS refuses 2.0+ data without it.
 * @return        true when the BIOS reports success (AX == 0x004F).
 * @note          The 512-byte buffer contract is pinned by static_asserts
 *                in vesa.hpp.
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       boot_bios
 */
bool QueryControllerInfo(VbeInfoBlock* info);

/**
 * @brief         Reads one VBE mode-info block via INT 10h AX=4F01.
 *
 * @param[in]     mode   Raw mode number from the controller's mode list.
 * @param[out]    info   Mode-info block the BIOS fills.
 * @return        true when the BIOS reports success (AX == 0x004F).
 * @note          Individual modes may legitimately fail; callers skip
 *                them instead of aborting the probe loop.
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       boot_bios
 */
bool QueryModeInfo(unsigned short mode, ModeInfoBlock* info);

/**
 * @brief         Switches the display to a VBE mode via INT 10h AX=4F02.
 *
 * @param[in]     mode   Mode number to activate.
 * @return        true when the BIOS reports success (AX == 0x004F).
 * @note          The linear-framebuffer bit is OR-ed in here, matching
 *                the LFB geometry the probe loop validated.
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       boot_bios
 */
bool SetVideoMode(unsigned short mode);

}  // namespace cinux::boot::bios
