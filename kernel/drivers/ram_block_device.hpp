/**
 * @file kernel/drivers/ram_block_device.hpp
 * @brief RAMBlockDevice -- in-memory IBlockDevice for tests and ramdisks
 *
 * Backed by a plain kernel heap allocation: read_blocks / write_blocks are
 * memcpy over a contiguous byte array sized block_count * block_size.  No DMA,
 * no hardware -- the simplest concrete IBlockDevice, used by unit tests (and
 * later ramdisk mounts) to exercise block consumers without real storage.
 *
 * Allocation goes through Heap directly rather than new[]: the kernel operator
 * new returns nullptr on exhaustion (no exceptions), and operator delete[] is
 * not provided, so an explicit Heap::alloc / free pair keeps the failure path
 * clean (Error::OutOfMemory) and the release path matched.
 *
 * Move-only: the backing storage has a single owner.  Copying would double-free
 * on destruction.
 *
 * Namespace: cinux::drivers
 */

#pragma once

#include <cinux/expected.hpp>
#include <cstddef>
#include <cstdint>

#include "kernel/drivers/block_device.hpp"
#include "kernel/lib/string.hpp"
#include "kernel/mm/heap.hpp"

namespace cinux::drivers {

/**
 * @brief In-memory IBlockDevice (test stub / ramdisk backing)
 */
class RAMBlockDevice : public IBlockDevice {
public:
    /// Default device-block size: one 512-byte sector, matching ATA devices.
    static constexpr uint64_t kDefaultBlockSize = 512;

    /**
     * @brief Allocate @p block_count blocks of @p block_size bytes each
     * @param block_count Number of blocks (must be > 0)
     * @param block_size  Bytes per block (must be > 0)
     * @return The device, or Error::InvalidArgument / Error::OutOfMemory
     */
    static cinux::lib::ErrorOr<RAMBlockDevice> create(uint64_t block_count,
                                                      uint64_t block_size = kDefaultBlockSize) {
        if (block_count == 0 || block_size == 0) {
            return cinux::lib::Error::InvalidArgument;
        }
        uint64_t total = block_count * block_size;
        void*    raw   = cinux::mm::g_heap.alloc(static_cast<std::size_t>(total));
        if (raw == nullptr) {
            return cinux::lib::Error::OutOfMemory;
        }
        return RAMBlockDevice(block_count, block_size, static_cast<uint8_t*>(raw));
    }

    ~RAMBlockDevice() override {
        if (storage_ != nullptr) {
            cinux::mm::g_heap.free(storage_);
        }
    }

    RAMBlockDevice(RAMBlockDevice&& other) noexcept
        : block_count_(other.block_count_),
          block_size_(other.block_size_),
          storage_(other.storage_) {
        other.block_count_ = 0;
        other.block_size_  = 0;
        other.storage_     = nullptr;
    }

    RAMBlockDevice& operator=(RAMBlockDevice&& other) noexcept {
        if (this != &other) {
            if (storage_ != nullptr) {
                cinux::mm::g_heap.free(storage_);
            }
            block_count_       = other.block_count_;
            block_size_        = other.block_size_;
            storage_           = other.storage_;
            other.block_count_ = 0;
            other.block_size_  = 0;
            other.storage_     = nullptr;
        }
        return *this;
    }

    RAMBlockDevice(const RAMBlockDevice&)            = delete;
    RAMBlockDevice& operator=(const RAMBlockDevice&) = delete;

    cinux::lib::ErrorOr<void> read_blocks(uint64_t block, uint64_t count, void* buf) override {
        if (block >= block_count_ || count > block_count_ - block) {
            return cinux::lib::Error::InvalidArgument;
        }
        if (count != 0) {
            memcpy(buf, storage_ + block * block_size_,
                   static_cast<std::size_t>(count * block_size_));
        }
        return {};
    }

    cinux::lib::ErrorOr<void> write_blocks(uint64_t block, uint64_t count,
                                           const void* buf) override {
        if (block >= block_count_ || count > block_count_ - block) {
            return cinux::lib::Error::InvalidArgument;
        }
        if (count != 0) {
            memcpy(storage_ + block * block_size_, buf,
                   static_cast<std::size_t>(count * block_size_));
        }
        return {};
    }

    uint64_t block_count() const override { return block_count_; }
    uint64_t block_size() const override { return block_size_; }

private:
    RAMBlockDevice(uint64_t block_count, uint64_t block_size, uint8_t* storage)
        : block_count_(block_count), block_size_(block_size), storage_(storage) {}

    uint64_t block_count_ = 0;
    uint64_t block_size_  = 0;
    uint8_t* storage_     = nullptr;
};

}  // namespace cinux::drivers
