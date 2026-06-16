/**
 * @file kernel/drivers/ahci/ahci_block_device.cpp
 * @brief AHCIBlockDevice implementation
 */

#include "ahci_block_device.hpp"

#include <cstddef>
#include <cstdint>

#include "kernel/drivers/dma/dma_pool.hpp"
#include "kernel/lib/string.hpp"

namespace cinux::drivers::ahci {

namespace {

// One page holds 8 sectors -- enough for any single ext2 block (up to 4096 B).
// Transfers larger than this are rejected rather than chunked; M4 is the
// minimal synchronous path.
constexpr uint64_t kDmaBufferSize = 4096;

}  // namespace

cinux::lib::ErrorOr<AHCIBlockDevice> AHCIBlockDevice::create(AHCI& ahci, uint8_t port_index,
                                                             uint64_t capacity_blocks) {
    auto buf = dma::g_dma_pool.alloc(kDmaBufferSize);
    if (!buf.ok()) {
        return buf.error();
    }
    return AHCIBlockDevice(&ahci, port_index, capacity_blocks, std::move(buf.value()));
}

AHCIBlockDevice::AHCIBlockDevice(AHCI* ahci, uint8_t port_index, uint64_t capacity_blocks,
                                 cinux::drivers::dma::DmaBuffer&& dma_buf)
    : ahci_(ahci),
      port_index_(port_index),
      capacity_blocks_(capacity_blocks),
      dma_buf_(std::move(dma_buf)) {}

cinux::lib::ErrorOr<void> AHCIBlockDevice::read_blocks(uint64_t block, uint64_t count, void* buf) {
    if (count == 0) {
        return {};
    }
    if (!dma_buf_.valid()) {
        return cinux::lib::Error::IOError;  // no DMA buffer (create-time alloc failed)
    }
    const uint64_t bytes = count * SECTOR_SIZE;
    if (bytes > dma_buf_.size()) {
        return cinux::lib::Error::InvalidArgument;  // transfer exceeds adapter buffer
    }
    if (!ahci_->read(port_index_, block, static_cast<uint16_t>(count), dma_buf_.phys())) {
        return cinux::lib::Error::IOError;
    }
    memcpy(buf, dma_buf_.virt(), static_cast<std::size_t>(bytes));
    return {};
}

cinux::lib::ErrorOr<void> AHCIBlockDevice::write_blocks(uint64_t block, uint64_t count,
                                                        const void* buf) {
    if (count == 0) {
        return {};
    }
    if (!dma_buf_.valid()) {
        return cinux::lib::Error::IOError;
    }
    const uint64_t bytes = count * SECTOR_SIZE;
    if (bytes > dma_buf_.size()) {
        return cinux::lib::Error::InvalidArgument;
    }
    memcpy(dma_buf_.virt(), buf, static_cast<std::size_t>(bytes));
    if (!ahci_->write(port_index_, block, static_cast<uint16_t>(count), dma_buf_.phys())) {
        return cinux::lib::Error::IOError;
    }
    return {};
}

}  // namespace cinux::drivers::ahci
