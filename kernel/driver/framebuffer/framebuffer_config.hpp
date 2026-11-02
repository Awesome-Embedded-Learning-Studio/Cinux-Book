/**
 * @file    framebuffer_config.hpp
 * @brief   The fbdev wire vocabulary: the two geometry records a
 *          framebuffer's device node answers with, Linux-shaped.
 *
 * These layouts mirror Linux's fb_var_screeninfo and fb_fix_screeninfo
 * byte for byte on this machine, because the asking side is a user
 * program compiled against its libc's linux/fb.h — the ABI is the
 * contract, and the sizes below are pinned by static assertions so a
 * layout drift is a build break, not a runtime surprise.
 *
 * @author  Charliechen114514
 * @date    2026-10-26
 * @version 0.1
 * @since   0.9.0
 * @ingroup kernel_driver
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace cinux::driver {

/// The question word for the changeable geometry record, FBIOGET_VSCREENINFO.
inline constexpr unsigned long long kFbGetVarScreeninfo = 0x4600;

/// The question word for the fixed geometry record, FBIOGET_FSCREENINFO.
inline constexpr unsigned long long kFbGetFixScreeninfo = 0x4602;

/// Bytes in the changeable geometry record, Linux's x86-64 count.
inline constexpr unsigned long kFbVarScreeninfoBytes = 160;

/// Bytes in the fixed geometry record, Linux's x86-64 count.
inline constexpr unsigned long kFbFixScreeninfoBytes = 80;

/// One color channel's place in the pixel word, the fb_bitfield shape.
struct FbBitfieldWire {
    uint32_t offset;     ///< First bit of the channel, from the word's low end.
    uint32_t length;     ///< Bits the channel owns.
    uint32_t msb_right;  ///< Set when bit zero names the most significant bit.
};

/// The changeable half of the framebuffer's facts: resolution, layout,
/// and channel placement, as the asking program expects them.
struct FbVarScreeninfoWire {
    uint32_t       xres;
    uint32_t       yres;
    uint32_t       xres_virtual;
    uint32_t       yres_virtual;
    uint32_t       xoffset;
    uint32_t       yoffset;
    uint32_t       bits_per_pixel;
    uint32_t       grayscale;
    FbBitfieldWire red;
    FbBitfieldWire green;
    FbBitfieldWire blue;
    FbBitfieldWire transp;
    uint32_t       nonstd;
    uint32_t       activate;
    uint32_t       height;
    uint32_t       width;
    uint32_t       accel_flags;
    uint32_t       pixclock;
    uint32_t       left_margin;
    uint32_t       right_margin;
    uint32_t       upper_margin;
    uint32_t       lower_margin;
    uint32_t       hsync_len;
    uint32_t       vsync_len;
    uint32_t       sync;
    uint32_t       vmode;
    uint32_t       rotate;
    uint32_t       colorspace;
    uint32_t       reserved[4];
};

/// The fixed half: where the pixels live and how rows stride, in the
/// asking program's word sizes.
struct FbFixScreeninfoWire {
    char     id[16];
    uint64_t smem_start;
    uint32_t smem_len;
    uint32_t type;
    uint32_t type_aux;
    uint32_t visual;
    uint16_t xpanstep;
    uint16_t ypanstep;
    uint16_t ywrapstep;
    uint32_t line_length;
    uint64_t mmio_start;
    uint32_t mmio_len;
    uint32_t accel;
    uint16_t capabilities;
};

static_assert(sizeof(FbVarScreeninfoWire) == kFbVarScreeninfoBytes,
              "the changeable record keeps Linux's x86-64 shape");
static_assert(sizeof(FbFixScreeninfoWire) == kFbFixScreeninfoBytes,
              "the fixed record keeps Linux's x86-64 shape");
static_assert(offsetof(FbFixScreeninfoWire, smem_start) == 16,
              "the identity block keeps its sixteen bytes");
static_assert(offsetof(FbFixScreeninfoWire, line_length) == 48,
              "the fixed record's padding stays where Linux put it");
static_assert(offsetof(FbVarScreeninfoWire, bits_per_pixel) == 24,
              "the depth word keeps Linux's offset");
static_assert(offsetof(FbVarScreeninfoWire, reserved) == 144,
              "the tail reservation keeps Linux's offset");

}  // namespace cinux::driver
