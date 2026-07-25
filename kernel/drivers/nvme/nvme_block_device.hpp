/**
 * @file kernel/drivers/nvme/nvme_block_device.hpp
 * @brief NvmeBlockDevice -- IBlockDevice adapter over the NVMe driver (F5-M3 batch 4c)
 *
 * Wraps an NVMe namespace behind the device-agnostic IBlockDevice interface so a
 * filesystem (ext2) or the perf harness speaks read_blocks/write_blocks and
 * never touches NVMe primitives (PRP, doorbells, queue IDs).
 *
 * Modelled on AHCIBlockDevice (F1-M4).  The adapter owns one 4 KiB DmaBuffer
 * (allocated from the DmaPool at create() time); each transfer DMAs into / out
 * of that buffer's phys via NvmeController::read_blocks/write_blocks, then
 * copies between virt() and the caller's buf.  The buffer is one memory page --
 * enough for any single ext2 block (up to 4096 B); larger counts are rejected
 * rather than chunked (PRP list for >1 page is a follow-up).
 *
 * Coexistence.  Production rootfs stays on AHCI; NvmeBlockDevice is the
 * independent second disk used by the batch-5 perf comparison (and any future
 * NVMe-backed Ext2).
 *
 * Namespace: cinux::drivers::nvme
 */

#pragma once

#include <cinux/expected.hpp>
#include <cstdint>
#include <utility>

#include "kernel/drivers/block_device.hpp"
#include "kernel/drivers/dma/dma_buffer.hpp"
#include "kernel/proc/sync.hpp"
#include "nvme.hpp"

namespace cinux::drivers::nvme {

/**
 * @brief IBlockDevice adapter over one NVMe namespace
 */
class NvmeBlockDevice : public cinux::drivers::IBlockDevice {
public:
    /**
     * @brief Create an adapter bound to @p ctrl namespace @p nsid
     * @param capacity_blocks  Namespace size in LBA blocks (from Identify Namespace nsze)
     * @param lba_size         Bytes per LBA (from Identify Namespace lba_size)
     * @return The adapter, or Error::OutOfMemory if the DMA buffer cannot be allocated.
     */
    static cinux::lib::ErrorOr<NvmeBlockDevice> create(NvmeController& ctrl, uint32_t nsid,
                                                       uint64_t capacity_blocks,
                                                       uint64_t lba_size);

    NvmeBlockDevice(NvmeBlockDevice&& other) noexcept            = default;
    NvmeBlockDevice& operator=(NvmeBlockDevice&& other) noexcept = default;
    NvmeBlockDevice(const NvmeBlockDevice&)                      = delete;
    NvmeBlockDevice& operator=(const NvmeBlockDevice&)           = delete;
    ~NvmeBlockDevice() override                                  = default;

    cinux::lib::ErrorOr<void> read_blocks(uint64_t block, uint64_t count, void* buf) override;
    cinux::lib::ErrorOr<void> write_blocks(uint64_t block, uint64_t count,
                                           const void* buf) override;
    // flush(): inherit IBlockDevice default no-op. A real NVMe flush (Admin Flush,
    // opcode 0x09) is a follow-up; current NVM Write is non-volatile on QEMU.

    uint64_t block_count() const override { return capacity_blocks_; }
    uint64_t block_size() const override { return lba_size_; }

private:
    NvmeBlockDevice(NvmeController* ctrl, uint32_t nsid, uint64_t capacity_blocks,
                    uint64_t lba_size, cinux::drivers::dma::DmaBuffer&& dma_buf);

    NvmeController*                ctrl_;
    uint32_t                       nsid_;
    uint64_t                       capacity_blocks_;
    uint64_t                       lba_size_;
    cinux::drivers::dma::DmaBuffer dma_buf_;
    // Serialises read_blocks/write_blocks across SMP CPUs: the single dma_buf_
    // + io_submit polling path must not overlap (production -smp 2 + gcc fork
    // chain exposed DmaBuffer corruption -> garbage ext2 block numbers -> NVMe
    // CAP_EXCEEDED on a wild slba).
    cinux::proc::Spinlock          lock_;
};

/**
 * @name Production boot-disk accessor
 *
 * main.cpp Step 21a creates the NvmeBlockDevice once NVMe bring-up succeeds and
 * registers it via set_nvme_block_device().  init.cpp reads it through
 * nvme_block_device() to choose the boot disk at runtime (NVMe if its namespace
 * carries a valid ext2 fs, else AHCI) -- no #ifdef (§14).  Returns nullptr when
 * the NVMe controller is absent (no -device nvme) or bring-up failed.
 */
///@{
NvmeBlockDevice* nvme_block_device();
void             set_nvme_block_device(NvmeBlockDevice* bd);
///@}

}  // namespace cinux::drivers::nvme
