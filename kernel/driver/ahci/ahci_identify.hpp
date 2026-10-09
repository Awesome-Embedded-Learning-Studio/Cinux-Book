/**
 * @file    ahci_identify.hpp
 * @brief   Reading what the drive said in its identify words.
 *
 * The identify device command answers 256 words about the drive; these
 * readers pull the two facts this kernel uses — the 48-bit sector
 * ceiling and the model string. The words belong to ATA, not to the
 * host controller, so they parse apart from the register layout.
 *
 * @author  Charliechen114514
 * @date    2026-10-09
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_driver
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <stdint.h>

#include <array>

namespace cinux::driver {

/**
 * @brief         Reads the 48-bit sector ceiling out of identify words.
 *
 * @param[in]     words   The 256 identify words as the drive wrote them.
 * @return        Highest addressable sector plus one.
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       kernel_driver
 */
[[nodiscard]] uint64_t SectorCeilingFromIdentify(const std::array<uint16_t, 256>& words);

/**
 * @brief         Copies the model string out of identify words.
 *
 * @param[in]     words   The 256 identify words as the drive wrote them.
 * @param[out]    model   Forty bytes of room; the string lands NUL-ended.
 * @return        None
 * @note          The drive strings are byte-swapped per word, so the
 *                      copy un-swaps as it goes.
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       kernel_driver
 */
void ModelFromIdentify(const std::array<uint16_t, 256>& words, std::array<char, 41>& model);

/**
 * @brief         Decides whether these identify words admit the drive.
 *
 * @param[in]     words   The 256 identify words as the drive wrote them.
 * @return        True when the drive speaks what this driver drives:
 *                      plain ATA (not CFA, not incomplete), printable
 *                      non-blank model, LBA48 with the DMA and flush
 *                      features this driver issues, a sane sector
 *                      ceiling, 512-byte logical sectors, and a valid
 *                      checksum whenever the drive marks one.
 * @note          The admission gate: failing it means the drive does
 *                      not register as a block device, whatever the
 *                      command's error flags said.
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       kernel_driver
 */
[[nodiscard]] bool IdentifyAdmits(const std::array<uint16_t, 256>& words);

}  // namespace cinux::driver
