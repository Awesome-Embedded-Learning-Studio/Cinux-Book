/**
 * @file    vesa.hpp
 * @brief   VBE video-mode contract shared by boot and host tests.
 *
 * @author  Charliechen114514
 * @date    2026-09-29
 * @version 0.1
 * @since   0.1.0
 * @ingroup boot_vesa
 * @copyright Copyright (c) 2026
 */

#pragma once

namespace cinux::boot {

struct [[gnu::packed]] VbeInfoBlock {
    unsigned char  signature[4];       // "VBE2" must be written before the call
    unsigned short version;            // e.g. 0x0300
    unsigned char  reserved_06[0x08];  // OEM pointer + capabilities
    unsigned short mode_list_offset;   // far pointer to uint16 mode numbers ...
    unsigned short mode_list_segment;  // ... terminated by 0xFFFF
    unsigned short total_memory;       // in 64 KB units
    unsigned char  tail[512 - 0x14];
};

struct [[gnu::packed]] ModeInfoBlock {
    unsigned short attributes;  // bit 7 = LFB supported
    unsigned char  reserved_02[0x0E];
    unsigned short pitch;  // bytes per scan line
    unsigned short width;
    unsigned short height;
    unsigned char  reserved_16[0x03];  // char cell, planes
    unsigned char  bpp;
    unsigned char  banks;                   // 0x1A
    unsigned char  memory_model;            // 0x1B
    unsigned char  bank_size;               // 0x1C
    unsigned char  image_pages;             // 0x1D
    unsigned char  reserved_1e;             // 0x1E
    unsigned char  red_mask_size;           // 0x1F
    unsigned char  red_field_position;      // 0x20
    unsigned char  green_mask_size;         // 0x21
    unsigned char  green_field_position;    // 0x22
    unsigned char  blue_mask_size;          // 0x23
    unsigned char  blue_field_position;     // 0x24
    unsigned char  rsvd_mask_size;          // 0x25
    unsigned char  rsvd_field_position;     // 0x26
    unsigned char  direct_color_mode_info;  // 0x27
    unsigned int   framebuffer;             // physical LFB base
    unsigned char  tail[256 - 0x2C];
};

/**
 * @brief   Boot-side archive of the framebuffer after the mode switch.
 * @note    Packed on purpose: i386 aligns long long to 4, LP64 to 8, so an
 *          unpacked version is 20 bytes in the boot world and 24 in the host
 *          world. Station 06 folds this into the kernel BootInfo.
 * @since   0.1.0
 * @ingroup boot_vesa
 */
struct [[gnu::packed]] FrameBufferInfo {
    unsigned long long physical;
    unsigned int       pitch;
    unsigned short     width;
    unsigned short     height;
    unsigned char      bpp;
};

/** @brief VBE mode-number bit that requests the linear framebuffer. */
inline constexpr unsigned short kLinearFrameBufferFlag = 0x4000;

/** @brief Value that terminates the controller's mode-number list. */
inline constexpr unsigned short kModeListTerminator = 0xFFFF;

/**
 * @brief         Reports whether one BIOS-reported mode satisfies a display request.
 *
 * @param[in]     info    Mode-info block exactly as the BIOS filled it.
 * @param[in]     width   Requested width in pixels.
 * @param[in]     height  Requested height in pixels.
 * @param[in]     bpp     Requested bits per pixel.
 * @return        true only for a linear-framebuffer mode with matching
 *                geometry, depth, and a nonzero framebuffer address.
 * @note          Text and banked VGA modes fail the attributes bit 7 check;
 *                a set bit with a zero address is treated as broken.
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       boot_vesa
 */
constexpr bool MatchesRequest(const ModeInfoBlock& info, unsigned short width,
                              unsigned short height, unsigned char bpp) {
    return (info.attributes & 0x0080) != 0 && info.width == width && info.height == height &&
           info.bpp == bpp && info.framebuffer != 0;
}

/**
 * @brief   Unpacked sample values feeding MakeSample for compile-time tests.
 * @note    Not a wire format: lives only in C++ code across both compile
 *          worlds, so packing is neither required nor wanted.
 * @since   0.1.0
 * @ingroup boot_vesa
 */
struct ModeSample {
    unsigned short attributes;
    unsigned short pitch;
    unsigned short width;
    unsigned short height;
    unsigned char  bpp;
    unsigned int   framebuffer;
};

/**
 * @brief         Builds a ModeInfoBlock carrying only the consumed fields.
 *
 * @param[in]     sample   Field values to inject; every other byte stays
 *                         value-initialized.
 * @return        A mode-info block usable by MatchesRequest in constexpr
 *                and runtime contexts alike.
 * @note          GCC flags designated initializers that skip the array
 *                members, so the factory assigns fields after zero-init
 *                instead.
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       boot_vesa
 */
constexpr ModeInfoBlock MakeSample(const ModeSample& sample) {
    ModeInfoBlock info{};
    info.attributes  = sample.attributes;
    info.pitch       = sample.pitch;
    info.width       = sample.width;
    info.height      = sample.height;
    info.bpp         = sample.bpp;
    info.framebuffer = sample.framebuffer;
    return info;
}

}  // namespace cinux::boot
