/**
 * @file    image_disk.hpp
 * @brief   A block device over an in-memory volume image.
 *
 * The same face MockDisk has, over bytes a test baked itself — usually
 * an ext2 volume mkfs produced and this file loaded in, so a backend
 * can walk a real on-disk layout without any hardware in sight.
 *
 * @author  Charliechen114514
 * @date    2026-10-10
 * @version 0.1
 * @since   0.1.0
 * @ingroup test_mock
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <stdint.h>

#include <vector>

#include "cinux/memory.hpp"
#include "kernel/driver/block/block_config.hpp"
#include "kernel/driver/block/block_device_concept.hpp"

namespace cinux::test {

constexpr unsigned long kImageSector = cinux::driver::kSectorBytes;

/**
 * @brief   A RAM-backed disk over caller-owned image bytes.
 * @note    The vector is moved in and must be whole sectors long.
 * @since   0.1.0
 * @ingroup test_mock
 */
class ImageDisk {
public:
    explicit ImageDisk(std::vector<std::uint8_t> image) : image_(std::move(image)) {}

    /**
     * @brief Copies sectors from the mock image.
     * @param[in] first Starting sector.
     * @param[in] span Number of sectors.
     * @param[out] sink Destination bytes.
     * @return False on injected failure or an out-of-range request.
     */
    bool read_blocks(cinux::driver::Lba first, cinux::driver::BlockSpan span, uint8_t* sink) {
        ++reads_;
        if (fail_reads_ || reads_ > read_limit_) {
            return false;
        }
        const unsigned long kFirst = first.value * kImageSector;
        const unsigned long kBytes = span.value * kImageSector;
        if (kFirst + kBytes > image_.size()) {
            return false;
        }
        cinux::base::CopyBytes(sink, image_.data() + kFirst, kBytes);
        return true;
    }

    /**
     * @brief Copies sectors into the mock image.
     * @param[in] first Starting sector.
     * @param[in] span Number of sectors.
     * @param[in] source Source bytes.
     * @return False on injected failure or an out-of-range request.
     */
    bool write_blocks(cinux::driver::Lba first, cinux::driver::BlockSpan span,
                      const uint8_t* source) {
        ++writes_;
        if (fail_writes_ || good_writes_ >= fail_after_) {
            return false;
        }
        const unsigned long kFirst = first.value * kImageSector;
        const unsigned long kBytes = span.value * kImageSector;
        if (kFirst + kBytes > image_.size()) {
            return false;
        }
        cinux::base::CopyBytes(image_.data() + kFirst, source, kBytes);
        ++good_writes_;
        return true;
    }

    /** @brief Returns the image's sector count. @return Sector count. */
    uint64_t block_count() { return image_.size() / kImageSector; }

    /** @brief Returns the sector size. @return Bytes per sector. */
    unsigned long block_size() { return kImageSector; }

    /** @brief Records a flush request. @return False on injected failure. */
    bool flush() {
        ++flushes_;
        return !fail_flushes_;
    }

    /// @brief Device flush requests, including injected failures.
    [[nodiscard]] unsigned long flush_count() const { return flushes_; }

    /// @brief Controls device flush failure injection.
    void fail_flushes(bool fail) { fail_flushes_ = fail; }

    /// @brief Read requests reaching the device, including injected failures.
    [[nodiscard]] unsigned long read_count() const { return reads_; }
    /// @brief Write requests reaching the device, including injected failures.
    [[nodiscard]] unsigned long write_count() const { return writes_; }
    /// @brief Controls read failure injection.
    void                        fail_reads(bool fail) { fail_reads_ = fail; }
    /// @brief Limit successful read requests for preparation-failure tests.
    void                        fail_reads_from(unsigned long survived) { read_limit_ = survived; }
    /// @brief Controls write failure injection.
    void                        fail_writes(bool fail) { fail_writes_ = fail; }
    /// @brief Fails every write from the nth successful one onward.
    void                        fail_writes_from(unsigned long survived) { fail_after_ = survived; }
    /// @brief The image bytes as they stand now, failed writes included.
    [[nodiscard]] const std::vector<std::uint8_t>& image() const { return image_; }

private:
    std::vector<std::uint8_t> image_;
    unsigned long             reads_        = 0;
    unsigned long             read_limit_   = ~0UL;
    unsigned long             writes_       = 0;
    unsigned long             flushes_      = 0;
    unsigned long             good_writes_  = 0;
    unsigned long             fail_after_   = ~0UL;
    bool                      fail_reads_   = false;
    bool                      fail_writes_  = false;
    bool                      fail_flushes_ = false;
};

static_assert(cinux::driver::BlockDevice<ImageDisk>, "the image disk satisfies the face");

}  // namespace cinux::test
