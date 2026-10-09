/**
 * @file    mock_block_disk.hpp
 * @brief   An in-memory block device for queue tests.
 *
 * @author  Charliechen114514
 * @date    2026-10-09
 * @version 0.1
 * @since   0.1.0
 * @ingroup test_mock
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <stdint.h>

#include <array>

#include "cinux/memory.hpp"
#include "kernel/driver/block/block_device_concept.hpp"

namespace cinux::test {

constexpr unsigned long kMockDiskBlocks = 64;
constexpr unsigned long kMockDiskSector = 512;

/**
 * @brief   A RAM-backed disk satisfying the block-device face.
 * @note    Counts flushes so tests can assert the flushes really travelled.
 * @since   0.1.0
 * @ingroup test_mock
 */
class MockDisk {
public:
    bool read_blocks(cinux::driver::Lba first, cinux::driver::BlockSpan span, uint8_t* sink) {
        if (first.value + span.value > kMockDiskBlocks) {
            return false;
        }
        cinux::base::CopyBytes(sink, storage_.data() + (first.value * kMockDiskSector),
                               static_cast<unsigned long>(span.value) * kMockDiskSector);
        return true;
    }

    bool write_blocks(cinux::driver::Lba first, cinux::driver::BlockSpan span,
                      const uint8_t* source) {
        if (first.value + span.value > kMockDiskBlocks) {
            return false;
        }
        cinux::base::CopyBytes(storage_.data() + (first.value * kMockDiskSector), source,
                               static_cast<unsigned long>(span.value) * kMockDiskSector);
        return true;
    }

    uint64_t block_count() { return kMockDiskBlocks; }

    unsigned long block_size() { return kMockDiskSector; }

    bool flush() {
        ++flushes_;
        return true;
    }

    /// @brief Flush calls that reached this disk.
    [[nodiscard]] unsigned long flush_count() const { return flushes_; }

private:
    std::array<uint8_t, kMockDiskBlocks * kMockDiskSector> storage_ = {};
    unsigned long                                          flushes_ = 0;
};

static_assert(cinux::driver::BlockDevice<MockDisk>, "the mock disk satisfies the face");

}  // namespace cinux::test
