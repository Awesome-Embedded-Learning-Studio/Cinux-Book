/**
 * @file kernel/drivers/ahci/ahci_block_device.hpp
 * @brief AHCIBlockDevice -- IBlockDevice adapter over the AHCI SATA driver
 *
 * Wraps a single AHCI port behind the device-agnostic IBlockDevice interface so
 * filesystems (ext2, batch 3) speak to storage through read_blocks/write_blocks
 * and never touch SATA primitives, bus addresses, or DMA plumbing.
 *
 * DMA.  The adapter owns one DmaBuffer (allocated from the M3 DmaPool at
 * create() time).  Each transfer DMAs into / out of that buffer's phys address
 * (handed to AHCI::read/write), then copies between virt() and the caller's buf.
 * The buffer is one page (8 sectors) -- enough for any single ext2 block (up to
 * 4096 B); larger counts are rejected.  This deliberately does NOT touch
 * ahci.cpp itself -- the AHCI driver's internal ad-hoc DMA is migrated to the
 * DmaPool/PrdtBuilder in F5-M1.
 *
 * Capacity.  AHCI exposes no identify/geometry today, so block_count() returns
 * the value passed to create() (0 by default).  Real device sizing arrives with
 * ATA IDENTIFY in F5-M1.
 *
 * Namespace: cinux::drivers::ahci
 */

#pragma once

#include <cinux/expected.hpp>
#include <cstdint>
#include <utility>

#include "ahci.hpp"
#include "kernel/drivers/block_device.hpp"
#include "kernel/drivers/dma/dma_buffer.hpp"

namespace cinux::drivers::ahci {

/**
 * @brief IBlockDevice adapter over one AHCI port
 *
 * @see IBlockDevice for the contract; @see AHCI for the underlying driver.
 */
class AHCIBlockDevice : public cinux::drivers::IBlockDevice {
public:
    /**
     * @brief Create an adapter bound to @p ahci port @p port_index
     * @param capacity_blocks Device size in 512-byte blocks (0 if unknown; real
     *                        sizing arrives with ATA IDENTIFY in F5-M1)
     * @return The adapter, or Error::OutOfMemory if the DMA buffer cannot be
     *         allocated from the DmaPool.
     */
    static cinux::lib::ErrorOr<AHCIBlockDevice> create(AHCI& ahci, uint8_t port_index,
                                                       uint64_t capacity_blocks = 0);

    AHCIBlockDevice(AHCIBlockDevice&& other) noexcept            = default;
    AHCIBlockDevice& operator=(AHCIBlockDevice&& other) noexcept = default;
    AHCIBlockDevice(const AHCIBlockDevice&)                      = delete;
    AHCIBlockDevice& operator=(const AHCIBlockDevice&)           = delete;
    ~AHCIBlockDevice() override                                  = default;

    cinux::lib::ErrorOr<void> read_blocks(uint64_t block, uint64_t count, void* buf) override;
    cinux::lib::ErrorOr<void> write_blocks(uint64_t block, uint64_t count,
                                           const void* buf) override;
    cinux::lib::ErrorOr<void> flush() override;  // ATA FLUSH CACHE EXT via AHCI

    uint64_t block_count() const override { return capacity_blocks_; }
    uint64_t block_size() const override { return SECTOR_SIZE; }

private:
    AHCIBlockDevice(AHCI* ahci, uint8_t port_index, uint64_t capacity_blocks,
                    cinux::drivers::dma::DmaBuffer&& dma_buf);

    AHCI*                          ahci_;
    uint8_t                        port_index_;
    uint64_t                       capacity_blocks_;
    cinux::drivers::dma::DmaBuffer dma_buf_;
};

}  // namespace cinux::drivers::ahci
