/**
 * @file kernel/mini/big_kernel_loader.hpp
 * @brief Big Kernel Loader - Orchestrates Disk Read + ELF Loading
 *
 * This module ties together the ATA disk driver and ELF loader to load
 * the "big kernel" from disk into memory. The big kernel is a full-featured
 * kernel ELF binary stored on disk after the mini kernel's reserved sectors.
 *
 * Loading Strategy:
 *   1. The big kernel ELF binary resides on disk starting at BIG_KERNEL_LBA
 *   2. We read sectors into a staging buffer at BIG_KERNEL_LOAD_ADDR (0x1000000 = 16MB)
 *   3. We read a conservative maximum number of sectors (enough for any reasonable kernel)
 *   4. Once the full ELF is in memory, we call the ELF loader to parse and relocate it
 *   5. The ELF loader handles PT_LOAD segment placement and BSS zeroing
 *   6. We return the entry point address for the caller to jump to
 *
 * Memory Layout During Loading:
 *   - 0x1000000 (16MB): Staging buffer for the raw ELF binary from disk
 *   - After ELF parsing: PT_LOAD segments are placed at their physical target addresses
 *
 * @note The staging buffer at 0x1000000 must not overlap with the mini kernel
 *       (loaded at 0x20000) or any bootloader structures (below 0x10000).
 */

#pragma once

#include <stdint.h>

namespace cinux::mini::loader {

// ============================================================
// Constants
// ============================================================

/// Physical address where the mini kernel was loaded by the bootloader
constexpr uint64_t MINI_KERNEL_LOAD_ADDR = 0x20000;

/// Physical address where the big kernel ELF will be staged from disk
constexpr uint64_t BIG_KERNEL_LOAD_ADDR = 0x1000000;  // 16MB

/// LBA on disk where the big kernel ELF binary starts (sector offset)
/// This must match the disk image layout defined in scripts/build_image.sh
/// The mini kernel occupies sectors 16+ (up to ~416KB = 832 sectors),
/// so the big kernel starts after the mini kernel region.
/// Default: sector 848 (16 + 832 sectors for mini kernel)
constexpr uint64_t BIG_KERNEL_LBA = 848;

/// Maximum number of sectors to read for the big kernel (512 sectors = 256KB)
/// This is a conservative upper bound; the actual kernel may be smaller.
/// The ELF loader will only process the segments it finds.
constexpr uint16_t BIG_KERNEL_MAX_SECTORS = 512;

// ============================================================
// Big Kernel Loading
// ============================================================

/**
 * @brief Load the big kernel from disk and return its entry point
 *
 * This is the main entry point for the big kernel loading process.
 * It performs the following steps:
 *   1. Reads the big kernel ELF from disk (BIG_KERNEL_LBA) into the
 *      staging buffer at BIG_KERNEL_LOAD_ADDR
 *   2. Validates the ELF header
 *   3. Parses and loads all PT_LOAD segments to their target addresses
 *   4. Returns the entry point address for the caller to jump to
 *
 * @param disk_lba  Starting LBA sector of the big kernel on disk
 * @return Physical entry point address on success, 0 on failure
 *
 * @note The caller is responsible for:
 *       - Initializing the ATA driver before calling this function
 *       - Jumping to the returned entry point
 *       - The mini kernel's serial/kprintf will be unavailable after the jump
 */
uint64_t load_big_kernel(uint64_t disk_lba);

}  // namespace cinux::mini::loader
