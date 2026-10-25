/**
 * @file    elf_layout.hpp
 * @brief   The ELF64 static-image vocabulary and its one parser.
 *
 * Exec needs two questions answered: where do the file's bytes land in
 * the new world, where does the first instruction sit, and what may be
 * done once it runs. The ELF header answers all of them, and this file
 * turns raw bytes into those answers as pure arithmetic — no
 * allocation, no mapping, no I/O — so host tests drive the same parser
 * the kernel does over hand-built images. Two seats are served, one per
 * role: the program's fixed-base shape and the interpreter's shared
 * shape; a dynamic program asking for the program seat, a wrong
 * machine, or a truncated table all refuse the same way.
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

/// Program-header type carrying the interpreter path: a string the exec
/// path reads, never bytes the loader maps.
inline constexpr std::uint32_t kProgramInterp = 3;

/// Program-header type carrying the stack's requested terms; the exec
/// path reads it so the stack's promise matches the image's ask.
inline constexpr std::uint32_t kProgramGnuStack = 0x6474E551;

/// Program-header flag bit: the segment's pages may hold running code.
inline constexpr std::uint32_t kSegmentExec = 0x1;

/// Program-header flag bit: the segment's pages may be stored through.
inline constexpr std::uint32_t kSegmentWrite = 0x2;

/// Program-header flag bit: the segment's pages may be loaded from.
inline constexpr std::uint32_t kSegmentRead = 0x4;

/// The file type of a fixed-base image: the program seat's shape,
/// static or carrying a PT_INTERP for a dynamic runtime to finish.
inline constexpr std::uint16_t kTypeExecutable = 2;

/// The file type of a shared object: the interpreter seat's shape,
/// position-independent so any base serves it.
inline constexpr std::uint16_t kTypeShared = 3;

/// The one machine served: x86-64.
inline constexpr std::uint16_t kMachineX8664 = 0x3E;

/// Which seat an image is parsed for. The seats differ in one fact —
/// the file type they accept — and that fact decides which images
/// parse and which refuse.
enum class ImageRole : std::uint8_t {
    kProgram,      ///< The exec path's main image: fixed-base executable.
    kInterpreter,  ///< The image a PT_INTERP names: shared object.
};

/// One loadable segment, as the loader wants it spelled.
struct ElfSegment {
    std::uint64_t virtual_address;  ///< Where the segment lands, page-granular care needed.
    std::uint64_t file_offset;      ///< First file byte belonging to it.
    std::uint64_t file_bytes;       ///< Bytes from the file.
    std::uint64_t memory_bytes;     ///< Bytes in memory; the tail past file_bytes is bss.
    std::uint32_t flags;            ///< Access terms: kSegmentRead/Write/Exec bits.
};

/// What one well-formed image reduces to.
struct ElfImage {
    std::uint64_t       entry;            ///< First instruction's address.
    std::uint64_t       headers_address;  ///< Where the program-header table lands in memory at a
                                          ///< zero base; zero when no loadable segment covers it.
    const std::uint8_t* segments;         ///< Pointer to the program-header table inside the file.
    std::uint16_t       segment_count;    ///< Loadable candidates in the table.
    std::uint16_t       segment_size;     ///< Bytes per program-header entry.
};

/**
 * @brief         Judges one buffer as an ELF64 image for one seat.
 *
 * @param[in]     data    The image bytes.
 * @param[in]     bytes   How many are readable at data.
 * @param[in]     role    Which seat the image is asked to fill; the
 *                        seat decides the accepted file type.
 * @return        The image reduced to entry, header-table address, and
 *                segment table, or kInvalidArgument for anything this
 *                loader refuses.
 * @note          Checks the magic, class, endianness, machine, the
 *                type the role asks for, and that the whole header and
 *                program-header table sit inside the buffer — every
 *                refusal names a reason in one line on the serial path
 *                in the caller.
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       kernel_loader
 */
[[nodiscard]] base::Result<ElfImage> ParseElf64(const std::uint8_t* data, std::size_t bytes,
                                                ImageRole role);

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

/**
 * @brief         Reads the interpreter path a program image names, if it
 *                names one.
 * @param[in]     data        The image bytes.
 * @param[in]     bytes       How many are readable at data.
 * @param[out]    path        Receives the NUL-terminated path.
 * @param[in]     path_bytes  Room at path, including the terminator.
 * @return        True when the program seat accepts the image and one
 *                PT_INTERP entry's file range fits inside the buffer;
 *                false for a static image, a refused image, or an
 *                entry that runs past the file.
 * @note          The segment carries the path as its bytes, so the copy
 *                comes straight from the buffer the caller already
 *                holds; the terminator is forced, so a linker that
 *                forgot its NUL still yields a bounded string.
 * @warning       None
 * @throws        None
 * @since         0.4.0
 * @ingroup       kernel_loader
 */
[[nodiscard]] bool ReadInterpreterPath(const std::uint8_t* data, std::size_t bytes, char* path,
                                       std::size_t path_bytes);

/**
 * @brief         Reads the stack terms a program image asks for, if it
 *                asks at all.
 * @param[in]     data    The image bytes.
 * @param[in]     bytes   How many are readable at data.
 * @param[out]    flags   Receives the PT_GNU_STACK entry's flag word.
 * @return        True when the program seat accepts the image and one
 *                PT_GNU_STACK entry sits inside the header table; false
 *                for an image that never states stack terms.
 * @note          Absence is an answer, not an error: an image without
 *                the entry leaves the caller's default standing. The
 *                exec bit in the returned word is the one question the
 *                exec path asks of it.
 * @warning       None
 * @throws        None
 * @since         0.4.0
 * @ingroup       kernel_loader
 */
[[nodiscard]] bool ReadGnuStackFlags(const std::uint8_t* data, std::size_t bytes,
                                     std::uint32_t& flags);

}  // namespace cinux::loader
