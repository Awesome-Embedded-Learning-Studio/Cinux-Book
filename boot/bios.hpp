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

}  // namespace cinux::boot::bios
