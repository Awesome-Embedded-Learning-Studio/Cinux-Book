/**
 * @file    boot_info.hpp
 * @brief   The boot-to-kernel handoff record.
 *
 * One packed struct crosses the world switch: what the kernel needs to
 * know about memory, the framebuffer, and itself. Packed on purpose —
 * the boot world writes it in 32-bit shapes, the kernel reads it in
 * 64-bit shapes, so padding must not differ between them.
 *
 * @author  Charliechen114514
 * @date    2026-10-01
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_boot
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <stdint.h>

namespace cinux::boot {

/** @brief E820 entries the BootInfo archive can carry. */
inline constexpr uint32_t kBootInfoE820Max = 128;

/// One memory-map entry as the kernel receives it.
struct [[gnu::packed]] E820Entry {
    uint64_t base;
    uint64_t length;
    uint32_t type;
};

/// The framebuffer geometry and color layout the kernel takes over.
struct [[gnu::packed]] FramebufferInfo {
    uint64_t physical;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint32_t bpp;
    uint32_t red_size, red_shift, green_size, green_shift, blue_size, blue_shift;
};

/// The whole handoff record; carried by pointer through the info mailbox.
struct [[gnu::packed]] BootInfo {
    uint32_t        magic;
    uint32_t        version;
    uint32_t        struct_size;
    uint32_t        e820_count;
    E820Entry       e820[kBootInfoE820Max];
    FramebufferInfo framebuffer;
    uint64_t        kernel_paddr;
    uint64_t        kernel_file_size;
    uint64_t        kernel_mem_size;
    uint64_t        kernel_entry;
};

/** @brief Marker the kernel checks before trusting the record. */
inline constexpr uint32_t kBootInfoMagic = 0x00114514;

/** @brief Record layout version; bumped when fields change meaning. */
inline constexpr uint32_t kBootInfoVersion = 1;

}  // namespace cinux::boot
