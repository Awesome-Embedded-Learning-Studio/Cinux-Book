/**
 * @file    image_header.hpp
 * @brief   Kernel image header and its load-time validation contract.
 *
 * The on-disk header is the treaty between the build and the loader: the
 * link scripts stamp it, stage2 judges the image by it before moving a
 * single sector, and both sides include this one file so the treaty never
 * drifts.
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

/// Half-open physical interval [base, top) used by placement decisions.
struct [[gnu::packed]] Region {
    uint64_t base;
    uint64_t top;
};

/// The on-disk header the link scripts stamp ahead of the kernel image.
struct [[gnu::packed]] ImageHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;
    uint64_t load_paddr;
    uint64_t file_size;
    uint64_t mem_size;
    uint64_t entry;
};

/** @brief Image magic the build stamps and the loader demands. */
inline constexpr uint32_t kImageMagic = 0x5A4B4E43;

/** @brief Header version this loader understands. */
inline constexpr uint16_t kImageVersion = 1;

/** @brief Highest physical address the handoff doors cover; the loader
 *         refuses images ending above it. */
inline constexpr uint64_t kHandoffDoorTop = 0x40000000ULL;

/**
 * @brief   Why a kernel image was refused, in check order.
 * @note    Each value maps to one rejected clause of ValidateImage, plus
 *          the two runtime outcomes of the ferry itself.
 * @since   0.1.0
 * @ingroup kernel_boot
 */
enum class LoadStatus : unsigned char {
    kOk,
    kBadMagic,
    kBadVersion,
    kEmptyImage,
    kBadSizes,
    kPaddrOverflow,
    kBeyondDoors,
    kEntryOutside,
    kNotInUsable,
    kBootOverlap,
    kDiskError,
    kMagicMismatch,
};

/**
 * @brief         Reports whether some region fully contains the interval.
 *
 * @param[in]     regions   Region array to search.
 * @param[in]     count     Number of regions.
 * @param[in]     base      Interval start, inclusive.
 * @param[in]     top       Interval end, exclusive.
 * @return        true when one region spans the whole interval.
 * @note          None
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       kernel_boot
 */
constexpr bool ContainsRegion(const Region* regions, unsigned count, uint64_t base, uint64_t top) {
    for (unsigned i = 0; i < count; ++i) {
        if (regions[i].base <= base && top <= regions[i].top) {
            return true;
        }
    }
    return false;
}

/**
 * @brief         Reports whether the interval crosses any listed region.
 *
 * @param[in]     regions   Region array to search.
 * @param[in]     count     Number of regions.
 * @param[in]     base      Interval start, inclusive.
 * @param[in]     top       Interval end, exclusive.
 * @return        true when the interval and some region overlap.
 * @note          Half-open intervals: touching at the top edge is not
 *                an overlap.
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       kernel_boot
 */
constexpr bool OverlapsRegion(const Region* regions, unsigned count, uint64_t base, uint64_t top) {
    for (unsigned i = 0; i < count; ++i) {
        if (base < regions[i].top && regions[i].base < top) {
            return true;
        }
    }
    return false;
}

/**
 * @brief         Judges a kernel image against memory reality before load.
 *
 * @param[in]     header        Image header as read from disk.
 * @param[in]     usable        Regions E820 reports as usable memory.
 * @param[in]     usable_count  Number of usable regions.
 * @param[in]     boot_owned    Regions the boot chain already occupies.
 * @param[in]     owned_count   Number of owned regions.
 * @param[in]     max_paddr     Highest address the handoff doors cover;
 *                              defaults to kHandoffDoorTop.
 * @return        The first failing check as a LoadStatus, or kOk.
 * @note          Checks run cheapest-first; sizes and arithmetic before
 *                memory reality, so a bogus header costs no scans.
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       kernel_boot
 */
// NOLINTNEXTLINE(readability-function-size)
constexpr LoadStatus ValidateImage(const ImageHeader& header, const Region* usable,
                                   unsigned usable_count, const Region* boot_owned,
                                   unsigned owned_count, uint64_t max_paddr = kHandoffDoorTop) {
    if (header.magic != kImageMagic) {
        return LoadStatus::kBadMagic;
    }
    if (header.version != kImageVersion) {
        return LoadStatus::kBadVersion;
    }
    if (header.file_size == 0) {
        return LoadStatus::kEmptyImage;
    }
    if (header.mem_size < header.file_size) {
        return LoadStatus::kBadSizes;
    }
    if (header.load_paddr + header.mem_size < header.load_paddr ||
        header.file_size + 511ULL < header.file_size) {
        return LoadStatus::kPaddrOverflow;
    }
    uint64_t const kEnd = header.load_paddr + header.mem_size;
    if (kEnd > max_paddr) {
        return LoadStatus::kBeyondDoors;
    }
    if (header.entry < header.load_paddr || header.entry >= kEnd) {
        return LoadStatus::kEntryOutside;
    }
    if (!ContainsRegion(usable, usable_count, header.load_paddr, kEnd)) {
        return LoadStatus::kNotInUsable;
    }
    if (OverlapsRegion(boot_owned, owned_count, header.load_paddr, kEnd)) {
        return LoadStatus::kBootOverlap;
    }
    return LoadStatus::kOk;
}

/**
 * @brief         Human-readable name of a load verdict.
 *
 * @param[in]     status   Verdict to name.
 * @return        Short reason string for boot-time failure reports.
 * @note          None
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       kernel_boot
 */
constexpr const char* NameOf(LoadStatus status) {
    switch (status) {
    case LoadStatus::kOk:
        return "ok";
    case LoadStatus::kBadMagic:
        return "bad magic";
    case LoadStatus::kBadVersion:
        return "bad version";
    case LoadStatus::kEmptyImage:
        return "empty image";
    case LoadStatus::kBadSizes:
        return "mem_size < file_size";
    case LoadStatus::kPaddrOverflow:
        return "paddr arithmetic overflow";
    case LoadStatus::kBeyondDoors:
        return "beyond handoff doors";
    case LoadStatus::kEntryOutside:
        return "entry outside image";
    case LoadStatus::kNotInUsable:
        return "no single usable region contains image";
    case LoadStatus::kBootOverlap:
        return "overlaps boot footprint";
    case LoadStatus::kDiskError:
        return "disk read error";
    case LoadStatus::kMagicMismatch:
        return "delivery check failed";
    }
    return "unknown";
}

}  // namespace cinux::boot
