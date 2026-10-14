/**
 * @file    elf_layout.hpp
 * @brief   The ELF64 static-image vocabulary and its one parser.
 *
 * Exec needs exactly one question answered: where do the file's bytes
 * land in the new world, and where does the first instruction sit. The
 * ELF header answers both, and this file turns raw bytes into that
 * answer as pure arithmetic — no allocation, no mapping, no I/O — so
 * host tests drive the same parser the kernel does over hand-built
 * images. Only the static executive shape is served: a dynamic image
 * or a wrong machine is refused the same way a truncated one is,
 * because the musl station is where those questions earn answers.
 *
 * @author  Charliechen114514
 * @date    2026-10-12
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_loader
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include "cinux/result.hpp"

namespace cinux::loader {

/// Magic a static ELF64 image opens with, little-endian view.
inline constexpr std::uint8_t kElfMagic[4] = {0x7F, 'E', 'L', 'F'};

/// The one program-header type the loader maps: a loadable segment.
inline constexpr std::uint32_t kProgramLoad = 1;

/// The one file type served: a static executable image.
inline constexpr std::uint16_t kTypeExecutable = 2;

/// The one machine served: x86-64.
inline constexpr std::uint16_t kMachineX8664 = 0x3E;

/// One loadable segment, as the loader wants it spelled.
struct ElfSegment {
    std::uint64_t virtual_address;  ///< Where the segment lands, page-granular care needed.
    std::uint64_t file_offset;      ///< First file byte belonging to it.
    std::uint64_t file_bytes;       ///< Bytes from the file.
    std::uint64_t memory_bytes;     ///< Bytes in memory; the tail past file_bytes is bss.
};

/// What one well-formed image reduces to.
struct ElfImage {
    std::uint64_t       entry;          ///< First instruction's address.
    const std::uint8_t* segments;       ///< Pointer to the program-header table inside the file.
    std::uint16_t       segment_count;  ///< Loadable candidates in the table.
    std::uint16_t       segment_size;   ///< Bytes per program-header entry.
};

/**
 * @brief         Judges one buffer as a static ELF64 executable.
 *
 * @param[in]     data    The image bytes.
 * @param[in]     bytes   How many are readable at data.
 * @return        The image reduced to entry and segment table, or
 *                kInvalidArgument for anything this loader refuses.
 * @note          Checks the magic, class, endianness, machine, type,
 *                and that the whole header and program-header table
 *                sit inside the buffer — every refusal names a reason
 *                in one line on the serial path in the caller.
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       kernel_loader
 */
[[nodiscard]] base::Result<ElfImage> ParseElf64(const std::uint8_t* data, std::size_t bytes);

/**
 * @brief         Reads one segment out of a parsed table.
 *
 * @param[in]     image   A parse that succeeded.
 * @param[in]     index   Which program-header entry, 0..segment_count-1.
 * @param[out]    segment The loadable facts of that entry.
 * @return        true when the entry is a loadable segment and fit to read;
 *                false for other header types and out-of-range indices.
 * @note          The parser only hands back the table; this word walks
 *                it, so the caller loops segments without knowing the
 *                on-disk layout at all.
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       kernel_loader
 */
[[nodiscard]] bool ReadSegment(const ElfImage& image, std::uint16_t index, ElfSegment& segment);

}  // namespace cinux::loader
