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

    bool read_blocks(cinux::driver::Lba first, cinux::driver::BlockSpan span, uint8_t* sink) {
        ++reads_;
        if (fail_reads_) {
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

    bool write_blocks(cinux::driver::Lba first, cinux::driver::BlockSpan span,
                      const uint8_t* source) {
        ++writes_;
        if (fail_writes_) {
            return false;
        }
        const unsigned long kFirst = first.value * kImageSector;
        const unsigned long kBytes = span.value * kImageSector;
        if (kFirst + kBytes > image_.size()) {
            return false;
        }
        cinux::base::CopyBytes(image_.data() + kFirst, source, kBytes);
        return true;
    }

    uint64_t block_count() { return image_.size() / kImageSector; }

    unsigned long block_size() { return kImageSector; }

    bool flush() { return true; }

    /// @brief Read requests reaching the device, including injected failures.
    [[nodiscard]] unsigned long read_count() const { return reads_; }
    /// @brief Write requests reaching the device, including injected failures.
    [[nodiscard]] unsigned long write_count() const { return writes_; }
    /// @brief Controls read failure injection.
    void                        fail_reads(bool fail) { fail_reads_ = fail; }
    /// @brief Controls write failure injection.
    void                        fail_writes(bool fail) { fail_writes_ = fail; }

private:
    std::vector<std::uint8_t> image_;
    unsigned long             reads_       = 0;
    unsigned long             writes_      = 0;
    bool                      fail_reads_  = false;
    bool                      fail_writes_ = false;
};

static_assert(cinux::driver::BlockDevice<ImageDisk>, "the image disk satisfies the face");

}  // namespace cinux::test
