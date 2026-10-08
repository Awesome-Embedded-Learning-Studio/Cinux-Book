/**
 * @file    ustar.hpp
 * @brief   Reader for the ustar archive format the initrd ships in.
 *
 * A tiny walk over the classic tape format: 512-byte header blocks,
 * octal ASCII size fields, data padded to whole blocks, two zero blocks
 * at the end. The reader hands out one entry per call — name, size,
 * file-or-directory, and a view straight into the archive bytes. Files
 * and directories are the whole vocabulary; anything else in the
 * typeflag is refused loudly rather than guessed at.
 *
 * @author  Charliechen114514
 * @date    2026-10-07
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_fs
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <string_view>

#include "cinux/result.hpp"
#include "kernel/fs/vfs.hpp"

namespace cinux::fs {

/**
 * @brief   One archive entry: where it is and what it is.
 * @note    name and data view into the archive buffer itself; both live
 *          exactly as long as the buffer does.
 * @since   0.1.0
 * @ingroup kernel_fs
 */
struct UstarEntry {
    std::string_view name;                     ///< NUL-terminated name, viewed in place.
    unsigned long    size = 0;                 ///< Data bytes; directories carry zero.
    InodeType        type = InodeType::kFile;  ///< File or directory.
    std::string_view data;                     ///< The entry's bytes, viewed in place.
};

/**
 * @brief   Sequential cursor over a ustar archive in memory.
 * @note    The reader never copies: entries view the buffer it was given.
 *          A zero block ends the walk cleanly; anything truncated or
 *          malformed is kInvalidArgument, not a guess.
 * @since   0.1.0
 * @ingroup kernel_fs
 */
class UstarReader {
public:
    /**
     * @brief         Points the reader at an archive image.
     *
     * @param[in]     base   First byte of the archive.
     * @param[in]     limit  Bytes the image actually holds.
     * @return        None
     * @since         0.1.0
     * @ingroup       kernel_fs
     */
    UstarReader(const unsigned char* base, unsigned long limit);

    /**
     * @brief         Hands out the next file or directory entry.
     *
     * @param[out]    entry   The next entry, viewing the archive bytes.
     * @return        True with entry filled; false at the archive's
     *                clean end; kInvalidArgument on truncation or a
     *                malformed header.
     * @since         0.1.0
     * @ingroup       kernel_fs
     */
    [[nodiscard]] base::Result<bool> next(UstarEntry& entry);

private:
    const unsigned char* base_;
    unsigned long        limit_;
    unsigned long        cursor_ = 0;
    bool                 done_   = false;
};

}  // namespace cinux::fs
