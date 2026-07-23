/**
 * @file kernel/drivers/nvme/nvme.hpp
 * @brief NvmeController -- PCIe NVMe controller bring-up (F5-M3)
 *
 * Brings up a single NVMe controller discovered via PCI: enables Bus Master +
 * Memory Space, maps BAR0 into the MMIO window, and reads CAP/VS to confirm the
 * register window is live.  Modelled on XHCIController (instance + init(const
 * PCIDevice&)) -- NOT all-static, because NVMe carries per-controller mutable
 * state (admin/IO queues, doorbells, MSI-X).
 *
 * Batches:
 *   1  -- PCI find + BAR0 self-assign + map + CAP/VS read.
 *   2a -- controller enable (CC.EN <-> CSTS.RDY) + Admin SQ/CQ config.
 *   2b -- Identify Controller via the admin queue (doorbell + CQ poll).
 *   3  -- MSI-X (multi-instance MsixController @+0x74000; vector 0x41).
 *   4a -- Identify Namespace + Create IO queues + admin_submit helper.
 *   4b/c -- NVM Read/Write (PRP), IBlockDevice adapter.
 *
 * Namespace: cinux::drivers::nvme
 */

#pragma once

#include <stdint.h>

#include <cinux/expected.hpp>

#include "kernel/drivers/dma/dma_buffer.hpp"
#include "kernel/drivers/pci/msix.hpp"
#include "kernel/drivers/pci/pci.hpp"

namespace cinux::drivers::nvme {

/// Fixed IDT vector for the NVMe MSI-X interrupt (entry 0). Clear of xHCI
/// (0x40). Registered in irq_init; not fired until init_msi_x enables MSI-X.
constexpr uint8_t kNvmeIrqVector = 0x41;

/// Controller registers (NVMe 1.x, MMIO offset from BAR0).  Doorbell registers
/// live at BAR0 + 0x1000 + queue_id * (2 * stride) and are computed in enable()
/// from CAP.DSTRD; they are NOT part of this fixed struct.
struct NvmeRegs {
    uint32_t cap_lo;     ///< 0x00 CAP low  (MQES[15:0], CQR[16], AMS[18:17], TO[27:23], DSTRD[31:28])
    uint32_t cap_hi;     ///< 0x04 CAP high (CSS, BPS, MPS8, ...)
    uint32_t vs;         ///< 0x08 Version (MJR[31:16].MNR[15:8].TER[7:0]); e.g. 0x00010400 = NVMe 1.4
    uint32_t intms;      ///< 0x0C Interrupt Mask Set
    uint32_t intmc;      ///< 0x10 Interrupt Mask Clear
    uint32_t cc;         ///< 0x14 Controller Configuration
    uint32_t reserved0;  ///< 0x18
    uint32_t csts;       ///< 0x1C Controller Status (RDY[0], CFS[1], SHST[4:2])
    uint32_t nssr;       ///< 0x20 NVM Subsystem Reset
    uint32_t aqa;        ///< 0x24 Admin Queue Attributes (ASQS[27:16].ACQS[11:0], 0-based)
    uint32_t asq_lo;     ///< 0x28 Admin SQ Base Address (low 32)
    uint32_t asq_hi;     ///< 0x2C
    uint32_t acq_lo;     ///< 0x30 Admin CQ Base Address (low 32)
    uint32_t acq_hi;     ///< 0x34
};

/// Admin Submission Queue Entry (64 bytes).  NVMe 1.x command format.
struct NvmeCmd {
    uint8_t  opcode;     ///< 0x00 e.g. kAdminIdentify = 0x06
    uint8_t  flags;      ///< 0x01 FUSE + PSDT (0)
    uint16_t cid;        ///< 0x02 Command Identifier
    uint32_t nsid;       ///< 0x04 Namespace ID
    uint64_t reserved0;  ///< 0x08 CDW02-03 (reserved)
    uint64_t mptr;       ///< 0x10 Metadata Pointer
    uint64_t prp1;       ///< 0x18 PRP Entry 1
    uint64_t prp2;       ///< 0x20 PRP Entry 2
    uint32_t cdw10;      ///< 0x28
    uint32_t cdw11;      ///< 0x2C
    uint32_t cdw12;      ///< 0x30
    uint32_t cdw13;      ///< 0x34
    uint32_t cdw14;      ///< 0x38
    uint32_t cdw15;      ///< 0x3C
};
static_assert(sizeof(NvmeCmd) == 64, "NVMe SQE must be 64 bytes");

/// Admin Completion Queue Entry (16 bytes).
struct NvmeCqe {
    uint32_t cdw0;      ///< 0x00 command-specific
    uint32_t reserved;  ///< 0x04
    uint16_t sq_head;   ///< 0x08 SQ Head pointer when the command completed
    uint16_t sq_id;     ///< 0x0A SQ Identifier
    uint16_t cq_id;     ///< 0x0C CQ Identifier
    uint16_t status;    ///< 0x0E bit0 = phase tag, bits[15:1] = SC/SCT
};
static_assert(sizeof(NvmeCqe) == 16, "NVMe CQE must be 16 bytes");

/// Partial Identify Controller data (offset 0x00-0x3F).  The full structure is
/// 4 KiB; we read only the identifying fields the batch-2b mechanism test needs.
struct IdentifyController {
    uint16_t vid;     ///< 0x00 PCI Vendor ID
    uint16_t ssvid;   ///< 0x02 PCI Subsystem Vendor ID
    uint8_t  sn[20];  ///< 0x04 Serial Number (ASCII)
    uint8_t  mn[40];  ///< 0x18 Model Number (ASCII)
};
static_assert(sizeof(IdentifyController) == 64, "IdentifyController header (vid/ssvid/sn/mn)");

/// Decoded Identify Namespace fields (batch 4a).  The full structure is 4 KiB;
/// we read nsze (offset 0x00) + the active LBA format's data size.
struct NamespaceInfo {
    uint64_t nsze;      ///< 0x00 Namespace Size (total LBAs)
    uint32_t lba_size;  ///< Bytes per LBA (2^(lbaf[flbas] & 0xFFF))
};

class NvmeController {
public:
    /// Map BAR0 into the MMIO window, enable Bus Master + Memory Space, and read
    /// CAP/VS to confirm the register window is live.  Does NOT enable the
    /// controller -- call enable() (batch 2a).
    cinux::lib::ErrorOr<void> init(const pci::PCIDevice& dev);

    /// Enable the controller (CC.EN -> wait CSTS.RDY=1) and configure the Admin
    /// Submission/Completion queues + doorbell pointers.  Batch 2a.
    cinux::lib::ErrorOr<void> enable();

    /// Submit an Identify Controller command on the admin queue and poll the CQ
    /// for completion (batch 2b).  Fills @p out with the identify data.
    cinux::lib::ErrorOr<void> identify_controller(IdentifyController& out);

    /// Configure MSI-X (batch 3): map the Table/PBA at +0x74000/+0x75000 (the
    /// default +0x40000 is taken by xHCI), program entry 0 -> vector 0x41, and
    /// enable.  ISR install is deferred to production -- the test kernel keeps
    /// CPU interrupts off, so this only proves the Table maps + entry programs.
    cinux::lib::ErrorOr<void> init_msi_x();

    /// Identify Namespace @p nsid (batch 4a): fill nsze + lba_size via the admin
    /// queue (CNS=0x02).
    cinux::lib::ErrorOr<void> identify_namespace(uint32_t nsid, NamespaceInfo& out);

    /// Create IO SQ/CQ (batch 4a): Admin Create IO CQ (opcode 0x05) + Create IO
    /// SQ (opcode 0x01), qid=1, size 64.  Leaves the IO doorbells ready for
    /// batch 4b NVM Read/Write.
    cinux::lib::ErrorOr<void> create_io_queues();

    /// Read @p nlb LBAs starting at @p slba from namespace @p nsid into @p buf
    /// (batch 4b).  @p buf is a DMA buffer, page-aligned, holding at least
    /// nlb*lba_size bytes; only single-page transfers are supported (PRP1 only,
    /// PRP2 = 0 -- a PRP list for >1 page is a follow-up).
    cinux::lib::ErrorOr<void> read_blocks(uint32_t nsid, uint64_t slba, uint16_t nlb,
                                          cinux::drivers::dma::DmaBuffer& buf);

    /// Write @p nlb LBAs from @p buf to namespace @p nsid at @p slba (batch 4b).
    /// Same buffer constraints as read_blocks.
    cinux::lib::ErrorOr<void> write_blocks(uint32_t nsid, uint64_t slba, uint16_t nlb,
                                           cinux::drivers::dma::DmaBuffer& buf);

    bool present() const { return regs_ != nullptr; }

    /// CAP.MQES (0-based; the maximum queue size is MQES + 1).
    uint16_t mqes() const { return static_cast<uint16_t>(regs_->cap_lo & 0xFFFF); }

    /// Version register (MJR[31:16].MNR[15:8]); e.g. 0x00010400 = NVMe 1.4.
    uint32_t version() const { return regs_->vs; }

    volatile NvmeRegs* regs() const { return regs_; }

    /// MSI-X Table (entry 0 programmed by init_msi_x).  Test hook.
    volatile pci::msix::MsixTableEntry* msix_table() const { return msix_.table(); }

private:
    /// Submit @p cmd on the admin SQ and poll the admin CQ for its completion
    /// (phase flip).  Returns the status field (SC/SCT, 0 = success) or
    /// Error::TimedOut.  Shared by Identify/Create commands (batch 4a helper).
    cinux::lib::ErrorOr<uint16_t> admin_submit(const NvmeCmd& cmd);

    /// Submit @p cmd on the IO SQ (qid=1) and poll the IO CQ for completion
    /// (batch 4b).  Mirrors admin_submit but for the IO queue pair created by
    /// create_io_queues().
    cinux::lib::ErrorOr<uint16_t> io_submit(const NvmeCmd& cmd);

    /// Build + submit an NVM Read (opcode 0x02) or Write (0x01) command
    /// (batch 4b).  cdw10/11 = SLBA, cdw12[15:0] = nlb-1, PRP1 = buf.phys(),
    /// PRP2 = 0 (single-page transfers only).
    cinux::lib::ErrorOr<void> nvm_io(uint8_t opcode, uint32_t nsid, uint64_t slba,
                                     uint16_t nlb, cinux::drivers::dma::DmaBuffer& buf);

    volatile NvmeRegs* regs_ = nullptr;
    pci::PCIDevice     dev_{};  // saved for MSI-X config (bus/slot/func + BARs)
    // Admin SQ/CQ (4 KiB DMA each).  Batch 2a.
    cinux::drivers::dma::DmaBuffer admin_sq_buf_;
    cinux::drivers::dma::DmaBuffer admin_cq_buf_;
    // Admin doorbell pointers + ring state.  Batch 2b.
    volatile uint32_t* admin_sq_tdbell_ = nullptr;
    volatile uint32_t* admin_cq_hdbell_ = nullptr;
    uint32_t           admin_sq_tail_   = 0;
    uint32_t           admin_cq_head_   = 0;
    uint8_t            cq_phase_        = 1;  // NVMe CQ starts at phase = 1
    uint32_t           doorbell_stride_ = 0;
    // MSI-X (batch 3).  Second MsixController instance (xHCI owns the first).
    pci::msix::MsixCap        msix_cap_{};
    pci::msix::MsixController msix_;
    // IO SQ/CQ (batch 4a).  qid=1.
    cinux::drivers::dma::DmaBuffer io_sq_buf_;
    cinux::drivers::dma::DmaBuffer io_cq_buf_;
    volatile uint32_t*             io_sq_tdbell_ = nullptr;
    volatile uint32_t*             io_cq_hdbell_ = nullptr;
    uint32_t                       io_sq_tail_   = 0;
    uint32_t                       io_cq_head_   = 0;
    uint8_t                        io_cq_phase_  = 1;
};

}  // namespace cinux::drivers::nvme
