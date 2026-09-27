/**
 * @file    layout.hpp
 * @brief   Physical-mode layout facts shared by every boot stage.
 *
 * The constants here are contracts, not preferences: the BIOS always drops
 * the MBR at 0x7C00, stage2 always follows one sector behind, and the
 * debug console always answers on port 0xE9. Boot code and host-side
 * tooling include the same values so the image on disk and the code that
 * jumps into it never disagree. The static_assert pins the width those
 * segment:offset values rely on — real-mode arithmetic assumes a 16-bit
 * unsigned short, and a toolchain that widens it silently changes the
 * layout every consumer agreed on.
 *
 * @author  Charliechen114514
 * @date    2026-09-27
 * @version 0.1
 * @since   0.1.0
 * @ingroup boot_layout
 */

#pragma once

namespace cinux::boot {

static_assert(sizeof(unsigned short) == 2);

/// Linear address the BIOS loads the MBR's first sector to.
inline constexpr unsigned short kMbrBase       = 0x7C00;
/// Stage2 segment: zero keeps the same linear map the MBR lives in.
inline constexpr unsigned short kStage2LoadSeg = 0x0000;
/// 0x7C00 plus one 512-byte sector: stage2 sits directly behind the MBR.
inline constexpr unsigned short kStage2LoadOff = 0x7E00;
/// Stage2 size in 512-byte sectors, i.e. 2048 bytes total.
inline constexpr unsigned short kStage2Sectors = 4;

}  // namespace cinux::boot
