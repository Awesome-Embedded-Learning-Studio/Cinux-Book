/**
 * @file kernel/drivers/nvme/nvme.cpp
 * @brief NvmeController bring-up: BAR0 assign + PCI enable + map + CAP/VS read (F5-M3 batch 1)
 *
 * Modelled on XHCIController::init (PCI COMMAND enable -> g_vmm.map BAR0 ->
 * reinterpret_cast).  BAR0 is mapped at KMEM_MMIO+0x70000 (4 pages = 16 KB),
 * avoiding every existing MMIO sub-allocation (AHCI/xHCI/MSI-X/e1000/HPET).
 *
 * BAR self-assignment.  The CinuxOS PCI layer reads BARs but does not assign
 * them -- it relies on SeaBIOS, which configures AHCI/e1000 but NOT the QEMU
 * nvme controller (dev.bar[0] reads 0).  Reading CAP/VS through an unassigned
 * BAR maps phys 0x0 (low RAM) and returns garbage that still satisfies a naive
 * "> 0" assertion (a false-green the batch-1 mechanism test exists to catch).
 * NVMe therefore self-assigns BAR0 via the standard probe (write all-1s, read
 * back the size bits) before mapping.  A generic PCI BAR allocator is a future
 * PCI-subsystem task; this self-assign is scoped to NVMe.
 */

#include "kernel/drivers/nvme/nvme.hpp"

#include <stdint.h>

#include <utility>

#include "kernel/arch/x86_64/memory_layout.hpp"
#include "kernel/arch/x86_64/paging_config.hpp"
#include "kernel/drivers/dma/dma_pool.hpp"
#include "kernel/drivers/pci/pci_config.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/mm/vmm.hpp"

namespace cinux::drivers::nvme {

namespace {
// KMEM_MMIO sub-allocation for the NVMe BAR0 register window.  MUST NOT collide
// with existing slots (AHCI @+0x0, LAPIC @+0x10000, IOAPIC @+0x11000, xHCI BAR0
// @+0x20000, MSI-X @+0x40000, e1000 @+0x50000, HPET @+0x60000).  4 pages = 16 KB
// covers the controller register window + the admin doorbells (the per-queue
// doorbell stride is derived from CAP.DSTRD in batch 2).
constexpr uint64_t kNvmeMmioVirt = cinux::arch::KMEM_MMIO_BASE + 0x70000;
constexpr uint64_t kNvmeBarPages = 4;
constexpr uint64_t kPageSize     = 4096;
constexpr uint64_t kMmioFlags =
    cinux::arch::FLAG_PRESENT | cinux::arch::FLAG_WRITABLE | cinux::arch::FLAG_PCD;

// Fixed 16 KB-aligned slot in the QEMU 32-bit PCI MMIO window for BAR0
// self-assignment, clear of AHCI BAR5 @0xfebf1000.  NVMe BAR0 is a 64-bit BAR
// but the QEMU register window is < 4 GB, so a 32-bit assign suffices (the high
// 32 bits stay zero).
constexpr uint32_t kAssignedBar0 = 0xfeb40000;

// Admin queue geometry + enable constants (batch 2a).
constexpr uint16_t kAdminQSize = 64;       // Admin SQ/CQ entries (well under MQES+1=2048)
// IO queue geometry (batch 4a/4b).  qid=1, 64 entries (well under MQES+1=2048).
constexpr uint16_t kIoQSize = 64;
constexpr uint32_t kReadyIters = 1000000;  // CSTS.RDY spin budget (no IRQ until batch 3)
constexpr uint32_t kCstsRdy    = 0x1;      // CSTS.RDY bit
// CC (NVMe 1.4): IOCQES[23:20]=4 (16 B CQ entry), IOSQES[19:16]=6 (64 B SQ
// entry), EN[0]=1; MPS=0 (4 KiB), CSS=0 (NVM), AMS=0 (round robin), SHN=0.
// Field positions per NVMe spec / QEMU hw/nvme (CC_IOSQES_SHIFT=16,
// CC_IOCQES_SHIFT=20). The Admin queue uses fixed 64 B/16 B entries and
// ignores these bits, so a wrong shift here passes every Admin command
// (Identify, Create IO Cq pre-check) but fails Create IO Cq/SQ -- QEMU's
// nvme_create_cq checks iosqes/iocqes first and returns NVME_MAX_QSIZE_EXCEEDED
// (0x0102 | DNR -> CQE status_field 0x8205) on mismatch.
constexpr uint32_t kCcEnable   = (4u << 20) | (6u << 16) | 1u;

void zero_dma(void* p, uint64_t bytes) {
    auto b = static_cast<volatile uint64_t*>(p);
    for (uint64_t i = 0; i < bytes / 8; ++i)
        b[i] = 0;
}
}  // namespace

cinux::lib::ErrorOr<void> NvmeController::init(const pci::PCIDevice& dev) {
    dev_ = dev;  // save for MSI-X config (bus/slot/func + BARs)

    // 1. Ensure BAR0 has an MMIO address.  If the BIOS left it unassigned
    //    (dev.bar[0] == 0 -- SeaBIOS skips the QEMU nvme controller), probe the
    //    size and assign a fixed slot ourselves.
    uint64_t bar0 = dev.bar[0];
    if ((bar0 & pci::BAR_ADDR_MASK_32) == 0) {
        pci::PCI::pci_write(dev.bus, dev.slot, dev.func, pci::PciReg::BAR0, 0xFFFFFFFFu);
        pci::PCI::pci_write(dev.bus, dev.slot, dev.func, pci::PciReg::BAR1, 0xFFFFFFFFu);
        const uint32_t probe = pci::PCI::pci_read(dev.bus, dev.slot, dev.func, pci::PciReg::BAR0);
        const uint32_t size  = ~(probe & pci::BAR_ADDR_MASK_32) + 1;
        pci::PCI::pci_write(dev.bus, dev.slot, dev.func, pci::PciReg::BAR0, kAssignedBar0);
        pci::PCI::pci_write(dev.bus, dev.slot, dev.func, pci::PciReg::BAR1, 0);
        bar0               = kAssignedBar0;
        // read_bars() ran BEFORE the self-assign, so dev.bar[0] still holds the
        // stale 0.  Update the saved copy so init_msi_x() computes the MSI-X
        // Table phys from the real BAR0 (0xfeb40000), not 0+table_offset --
        // otherwise the Table maps to low RAM and program_vector() overwrites
        // it, corrupting nearby low-memory mappings (batch-3 #PF root cause).
        dev_.bar[0]        = kAssignedBar0;
        const uint32_t rb0 = pci::PCI::pci_read(dev.bus, dev.slot, dev.func, pci::PciReg::BAR0);
        const uint32_t rb1 = pci::PCI::pci_read(dev.bus, dev.slot, dev.func, pci::PciReg::BAR1);
        cinux::lib::kprintf(
            "[NVMe] BAR0 was unassigned (probe size=%u) -> 0x%x (rb0=0x%x rb1=0x%x)\n",
            static_cast<unsigned>(size), kAssignedBar0, rb0, rb1);
    }

    // 2. Enable PCI Bus Master + Memory Space so the controller may master DMA
    //    and expose its MMIO window.
    const uint32_t cmd = pci::PCI::pci_read(dev.bus, dev.slot, dev.func, pci::PciReg::COMMAND);
    pci::PCI::pci_write(dev.bus, dev.slot, dev.func, pci::PciReg::COMMAND,
                        cmd | pci::PciCmd::BUS_MASTER | pci::PciCmd::MEM_SPACE);

    // 3. Map BAR0 into the MMIO window (uncached).
    for (uint64_t i = 0; i < kNvmeBarPages; ++i) {
        if (!cinux::mm::g_vmm.map(kNvmeMmioVirt + i * kPageSize, bar0 + i * kPageSize,
                                  kMmioFlags)) {
            cinux::lib::kprintf("[NVMe] BAR0 map failed at page %u\n", static_cast<unsigned>(i));
            return cinux::lib::Error::IOError;
        }
    }
    regs_ = reinterpret_cast<volatile NvmeRegs*>(kNvmeMmioVirt);

    // 4. Read CAP/VS to confirm the window is live (mechanism test, batch 1).
    //    DSTRD = CAP bits[31:28] (NVMe 1.4); bits[27:24] are TO's high nibble.
    //    doorbell stride = 4 << DSTRD, computed in enable().
    const uint32_t cap_lo = regs_->cap_lo;
    const uint32_t vs     = regs_->vs;
    cinux::lib::kprintf("[NVMe] BAR0=0x%lx CAP.MQES=%u DSTRD=%u VS=%u.%u\n",
                        static_cast<unsigned long>(bar0), static_cast<unsigned>(cap_lo & 0xFFFF),
                        static_cast<unsigned>((cap_lo >> 28) & 0xF),
                        static_cast<unsigned>((vs >> 16) & 0xFFFF),
                        static_cast<unsigned>((vs >> 8) & 0xFF));  // MNR = VS bits[15:8]
    return {};
}

cinux::lib::ErrorOr<void> NvmeController::enable() {
    // 1. Disable (CC.EN=0 -> wait CSTS.RDY=0).  The QEMU controller boots
    //    disabled, but a clean disable also handles the "already enabled" case.
    regs_->cc = 0;
    for (uint32_t i = 0; i < kReadyIters; ++i) {
        if (!(regs_->csts & kCstsRdy)) {
            break;
        }
        if (i + 1 == kReadyIters) {
            cinux::lib::kprintf("[NVMe] disable timeout CSTS=0x%x\n", regs_->csts);
            return cinux::lib::Error::TimedOut;
        }
    }

    // 2. Allocate Admin SQ (kAdminQSize x 64 B) + CQ (kAdminQSize x 16 B).
    //    The controller requires MQES+1 >= queue size.
    if (mqes() + 1 < kAdminQSize) {
        cinux::lib::kprintf("[NVMe] MQES=%u < admin queue size %u\n", mqes(), kAdminQSize);
        return cinux::lib::Error::InvalidArgument;
    }
    auto sq_r = cinux::drivers::dma::g_dma_pool.alloc(static_cast<uint64_t>(kAdminQSize) * 64);
    auto cq_r = cinux::drivers::dma::g_dma_pool.alloc(static_cast<uint64_t>(kAdminQSize) * 16);
    if (!sq_r.ok() || !cq_r.ok()) {
        return cinux::lib::Error::OutOfMemory;
    }
    admin_sq_buf_ = std::move(sq_r.value());
    admin_cq_buf_ = std::move(cq_r.value());

    // 3. Zero both DMA buffers -- the controller reads CQ phase bits + SQ
    //    entries from phys.  x86 DMA is cache-coherent, so no flush is needed.
    zero_dma(admin_sq_buf_.virt(), admin_sq_buf_.size());
    zero_dma(admin_cq_buf_.virt(), admin_cq_buf_.size());

    // 4. Configure Admin Queue (AQA + ASQ + ACQ).  Size fields are 0-based.
    regs_->aqa =
        (static_cast<uint32_t>(kAdminQSize - 1) << 16) | static_cast<uint32_t>(kAdminQSize - 1);
    const uint64_t sq_phys = admin_sq_buf_.phys();
    const uint64_t cq_phys = admin_cq_buf_.phys();
    regs_->asq_lo          = static_cast<uint32_t>(sq_phys);
    regs_->asq_hi          = static_cast<uint32_t>(sq_phys >> 32);
    regs_->acq_lo          = static_cast<uint32_t>(cq_phys);
    regs_->acq_hi          = static_cast<uint32_t>(cq_phys >> 32);

    // 5. Enable (CC.EN=1, MPS=0 4 KiB, CSS=0 NVM, AMS=0 RR, IOCQES=4 16 B,
    //    IOSQES=6 64 B).
    regs_->cc = kCcEnable;

    // 6. Wait CSTS.RDY=1 (the controller processed CC.EN=1), then derive the
    //    admin doorbell pointers (batch 2b).  Admin SQ tail @ BAR0+0x1000,
    //    Admin CQ head @ BAR0+0x1000+stride (stride = 4 << DSTRD).
    for (uint32_t i = 0; i < kReadyIters; ++i) {
        if (regs_->csts & kCstsRdy) {
            doorbell_stride_     = 4u << ((regs_->cap_lo >> 28) & 0xF);
            const uintptr_t base = reinterpret_cast<uintptr_t>(regs_);
            admin_sq_tdbell_     = reinterpret_cast<volatile uint32_t*>(base + 0x1000);
            admin_cq_hdbell_ =
                reinterpret_cast<volatile uint32_t*>(base + 0x1000 + doorbell_stride_);
            cinux::lib::kprintf("[NVMe] enabled (admin queue=%u RDY=1 doorbell stride=%u)\n",
                                kAdminQSize, doorbell_stride_);
            return {};
        }
    }
    cinux::lib::kprintf("[NVMe] enable timeout CSTS=0x%x\n", regs_->csts);
    return cinux::lib::Error::TimedOut;
}

cinux::lib::ErrorOr<void> NvmeController::identify_controller(IdentifyController& out) {
    // 4 KiB identify data buffer (PRP1).
    auto buf_r = cinux::drivers::dma::g_dma_pool.alloc(4096);
    if (!buf_r.ok()) {
        return cinux::lib::Error::OutOfMemory;
    }
    cinux::drivers::dma::DmaBuffer buf = std::move(buf_r.value());
    zero_dma(buf.virt(), buf.size());

    // Build Identify Controller (opcode 0x06, CNS=0x01) at SQ[tail].
    auto*          sq             = static_cast<NvmeCmd*>(admin_sq_buf_.virt());
    NvmeCmd&       cmd            = sq[admin_sq_tail_];
    const uint8_t  kAdminIdentify = 0x06;
    const uint32_t kCnsController = 0x01;
    zero_dma(&cmd, sizeof(cmd));
    cmd.opcode = kAdminIdentify;
    cmd.nsid   = 0;
    cmd.prp1   = buf.phys();
    cmd.cdw10  = kCnsController;

    admin_sq_tail_    = (admin_sq_tail_ + 1) % kAdminQSize;
    *admin_sq_tdbell_ = admin_sq_tail_;  // ring Admin SQ tail doorbell

    // Poll the Admin CQ for this command's completion (phase tag flips).
    auto* cq = reinterpret_cast<volatile NvmeCqe*>(admin_cq_buf_.virt());
    for (uint32_t i = 0; i < kReadyIters; ++i) {
        volatile NvmeCqe& cqe          = cq[admin_cq_head_];
        const uint16_t    status_field = cqe.status;
        if ((status_field & 0x1) == cq_phase_) {
            const uint16_t status = static_cast<uint16_t>(status_field >> 1);  // SC/SCT
            admin_cq_head_        = (admin_cq_head_ + 1) % kAdminQSize;
            if (admin_cq_head_ == 0) {
                cq_phase_ ^= 1;  // phase wraps at CQ ring rollover
            }
            *admin_cq_hdbell_ = admin_cq_head_;
            if (status != 0) {
                cinux::lib::kprintf("[NVMe] Identify failed status=0x%x\n", status);
                return cinux::lib::Error::IOError;
            }
            const auto*    id  = static_cast<const IdentifyController*>(buf.virt());
            const auto*    raw = static_cast<const uint8_t*>(buf.virt());
            const uint32_t nn  = *reinterpret_cast<const uint32_t*>(raw + 516);
            out                = *id;
            cinux::lib::kprintf(
                "[NVMe] Identify: VID=0x%x SSVID=0x%x SN=%.20s MN=%.40s NN=%u\n", id->vid,
                id->ssvid, id->sn, id->mn, nn);
            return {};
        }
    }
    cinux::lib::kprintf("[NVMe] Identify timeout\n");
    return cinux::lib::Error::TimedOut;
}

cinux::lib::ErrorOr<void> NvmeController::init_msi_x() {
    msix_cap_ = pci::msix::find_capability(dev_.bus, dev_.slot, dev_.func, &pci::PCI::pci_read);
    if (!msix_cap_.found) {
        cinux::lib::kprintf("[NVMe] no MSI-X capability\n");
        return cinux::lib::Error::InvalidArgument;
    }
    // Second MsixController instance: map Table/PBA at +0x74000/+0x75000 (the
    // default +0x40000/+0x41000 is taken by xHCI).  Entry 0 -> vector 0x41
    // (xHCI owns 0x40); ISR install is deferred to production (batch 4) -- the
    // test kernel keeps CPU interrupts off, so this batch only proves the Table
    // maps + entry programs (mirroring xHCI batch 2C).
    constexpr uint64_t kNvmeMsixTableVirt = cinux::arch::KMEM_MMIO_BASE + 0x74000;
    constexpr uint64_t kNvmeMsixPbaVirt   = cinux::arch::KMEM_MMIO_BASE + 0x75000;
    auto               r = msix_.init(msix_cap_, dev_, kNvmeMsixTableVirt, kNvmeMsixPbaVirt);
    if (!r.ok()) {
        return cinux::lib::Error::OutOfMemory;
    }
    msix_.mask_all();
    msix_.program_vector(0, kNvmeIrqVector, 0);
    msix_.program_vector(1, kNvmeIrqVector, 0);  // batch 4: IO Cq IV
    msix_.enable();  // batch 4: IDT[kNvmeIrqVector] registered in irq_init
    // Re-mask every entry. The test kernel runs IF=0 and never calls
    // switch_to_apic(), so irq_eoi() still targets the 8259 PIC -- a no-op for
    // MSI-X. An unmasked admin-completion MSI (vector 0x41) would enter the
    // LAPIC IRR, be delivered on the next sti, run the ISR stub whose EOI is a
    // PIC no-op, and strand 0x41 in the LAPIC ISR. That raises the processor
    // priority above the LAPIC timer (vector 0x30) that e1000's ARP poll
    // sti+hlt depends on (the bug interrupts.S:264 warns about), hanging every
    // later e1000 test. Masked entries still verify the Table map + entry
    // programming (msg_addr != 0) + MC.Enable bit; production (batch 4c)
    // installs the ISR and switches to APIC before unmasking.
    msix_.mask_all();
    cinux::lib::kprintf("[NVMe] MSI-X enabled (entry 0 -> vector 0x%x, %u entries)\n",
                        kNvmeIrqVector, msix_cap_.table_size);
    return {};
}

cinux::lib::ErrorOr<uint16_t> NvmeController::admin_submit(const NvmeCmd& cmd) {
    // Enqueue on the admin SQ at [tail] and ring the doorbell.
    auto*      sq            = static_cast<NvmeCmd*>(admin_sq_buf_.virt());
    sq[admin_sq_tail_]       = cmd;
    admin_sq_tail_           = (admin_sq_tail_ + 1) % kAdminQSize;
    *admin_sq_tdbell_        = admin_sq_tail_;

    // Poll the admin CQ for this command's completion (phase tag flips).
    auto* cq = reinterpret_cast<volatile NvmeCqe*>(admin_cq_buf_.virt());
    for (uint32_t i = 0; i < kReadyIters; ++i) {
        volatile NvmeCqe& cqe          = cq[admin_cq_head_];
        const uint16_t    status_field = cqe.status;
        if ((status_field & 0x1) == cq_phase_) {
            const uint16_t status = static_cast<uint16_t>(status_field >> 1);
            admin_cq_head_        = (admin_cq_head_ + 1) % kAdminQSize;
            if (admin_cq_head_ == 0) {
                cq_phase_ ^= 1;
            }
            *admin_cq_hdbell_ = admin_cq_head_;
            return status;
        }
    }
    return cinux::lib::Error::TimedOut;
}

cinux::lib::ErrorOr<void> NvmeController::identify_namespace(uint32_t nsid, NamespaceInfo& out) {
    auto buf_r = cinux::drivers::dma::g_dma_pool.alloc(4096);
    if (!buf_r.ok()) {
        return cinux::lib::Error::OutOfMemory;
    }
    cinux::drivers::dma::DmaBuffer buf = std::move(buf_r.value());
    zero_dma(buf.virt(), buf.size());

    NvmeCmd cmd{};
    cmd.opcode = 0x06;  // Identify
    cmd.nsid   = nsid;
    cmd.prp1   = buf.phys();
    cmd.cdw10  = 0x00;  // CNS=0x00 (Identify Namespace, nsid-specific)
    auto sr    = admin_submit(cmd);
    if (!sr.ok()) {
        return sr.error();
    }
    if (sr.value() != 0) {
        cinux::lib::kprintf("[NVMe] Identify Namespace %u failed status=0x%x\n", nsid, sr.value());
        return cinux::lib::Error::IOError;
    }

    const auto*    data  = static_cast<const uint8_t*>(buf.virt());
    const uint8_t  flbas = data[0x1A];
    const uint32_t lbaf  = *reinterpret_cast<const uint32_t*>(data + 0x80 + (flbas & 0xF) * 4);
    out.nsze             = *reinterpret_cast<const uint64_t*>(data);
    out.lba_size         = 1u << ((lbaf >> 16) & 0xFF);  // LBADS = LBAF bits[23:16]
    cinux::lib::kprintf("[NVMe] Namespace %u: nsze=%llu lba_size=%u\n", nsid,
                        static_cast<unsigned long long>(out.nsze), out.lba_size);
    return {};
}

cinux::lib::ErrorOr<void> NvmeController::create_io_queues() {
    constexpr uint16_t kIoQid         = 1;

    auto sq_r = cinux::drivers::dma::g_dma_pool.alloc(static_cast<uint64_t>(kIoQSize) * 64);
    auto cq_r = cinux::drivers::dma::g_dma_pool.alloc(static_cast<uint64_t>(kIoQSize) * 16);
    if (!sq_r.ok() || !cq_r.ok()) {
        return cinux::lib::Error::OutOfMemory;
    }
    io_sq_buf_ = std::move(sq_r.value());
    io_cq_buf_ = std::move(cq_r.value());
    zero_dma(io_sq_buf_.virt(), io_sq_buf_.size());
    zero_dma(io_cq_buf_.virt(), io_cq_buf_.size());

    // Create IO CQ (opcode 0x05): PRP1=CQ phys, CDW10=QID|QSIZE, CDW11=IV|IEN|PC.
    {
        NvmeCmd cmd{};
        cmd.opcode = 0x05;
        cmd.prp1   = io_cq_buf_.phys();
        cmd.cdw10  = (static_cast<uint32_t>(kIoQSize - 1) << 16) | kIoQid;
        cmd.cdw11  = (1u << 16) | (1u << 1) | 1u;  // IV=1 (MSI-X entry 1), IEN=1, PC=1
        auto sr    = admin_submit(cmd);
        if (!sr.ok() || sr.value() != 0) {
            cinux::lib::kprintf("[NVMe] Create IO CQ failed status=%d\n", sr.ok() ? sr.value() : -1);
            return cinux::lib::Error::IOError;
        }
    }
    // Create IO SQ (opcode 0x01): PRP1=SQ phys, CDW10=QID|QSIZE, CDW11=CQID|QPRIO|PC.
    {
        NvmeCmd cmd{};
        cmd.opcode = 0x01;
        cmd.prp1   = io_sq_buf_.phys();
        cmd.cdw10  = (static_cast<uint32_t>(kIoQSize - 1) << 16) | kIoQid;
        cmd.cdw11  = (static_cast<uint32_t>(kIoQid) << 16) | 1u;  // CQID=1, QPRIO=0, PC
        auto sr    = admin_submit(cmd);
        if (!sr.ok() || sr.value() != 0) {
            cinux::lib::kprintf("[NVMe] Create IO SQ failed status=%d\n", sr.ok() ? sr.value() : -1);
            return cinux::lib::Error::IOError;
        }
    }

    // IO doorbells: SQ qid=1 tail @ BAR0+0x1000+2*1*stride, CQ qid=1 head @ +0x1000+3*stride.
    const uintptr_t base = reinterpret_cast<uintptr_t>(regs_);
    io_sq_tdbell_        = reinterpret_cast<volatile uint32_t*>(base + 0x1000 + 2 * doorbell_stride_);
    io_cq_hdbell_        = reinterpret_cast<volatile uint32_t*>(base + 0x1000 + 3 * doorbell_stride_);
    io_sq_tail_          = 0;
    io_cq_head_          = 0;
    io_cq_phase_         = 1;
    cinux::lib::kprintf("[NVMe] IO queues created (qid=1 size=%u)\n", kIoQSize);
    return {};
}

cinux::lib::ErrorOr<uint16_t> NvmeController::io_submit(const NvmeCmd& cmd) {
    // Enqueue on the IO SQ (qid=1) at [tail] and ring the doorbell.
    auto*   sq                = static_cast<NvmeCmd*>(io_sq_buf_.virt());
    sq[io_sq_tail_]           = cmd;
    io_sq_tail_               = (io_sq_tail_ + 1) % kIoQSize;
    *io_sq_tdbell_            = io_sq_tail_;

    // Poll the IO CQ for this command's completion (phase tag flips).
    auto* cq = reinterpret_cast<volatile NvmeCqe*>(io_cq_buf_.virt());
    for (uint32_t i = 0; i < kReadyIters; ++i) {
        volatile NvmeCqe& cqe          = cq[io_cq_head_];
        const uint16_t    status_field = cqe.status;
        if ((status_field & 0x1) == io_cq_phase_) {
            const uint16_t status = static_cast<uint16_t>(status_field >> 1);
            io_cq_head_           = (io_cq_head_ + 1) % kIoQSize;
            if (io_cq_head_ == 0) {
                io_cq_phase_ ^= 1;
            }
            *io_cq_hdbell_ = io_cq_head_;
            return status;
        }
    }
    return cinux::lib::Error::TimedOut;
}

cinux::lib::ErrorOr<void> NvmeController::nvm_io(uint8_t opcode, uint32_t nsid, uint64_t slba,
                                                 uint16_t                              nlb,
                                                 cinux::drivers::dma::DmaBuffer& buf) {
    // NVM Read (0x02) / Write (0x01) -- NvmeRwCmd layout (QEMU hw/nvme/nvme.h):
    //   cdw10/11 = SLBA (64-bit), cdw12[15:0] = NLB-1 (0-based), dptr = PRP1/PRP2.
    // Single-page transfer: PRP1 = buf.phys() (page-aligned), PRP2 = 0.
    NvmeCmd cmd{};
    cmd.opcode = opcode;
    cmd.nsid   = nsid;
    cmd.prp1   = buf.phys();
    cmd.prp2   = 0;
    cmd.cdw10  = static_cast<uint32_t>(slba);
    cmd.cdw11  = static_cast<uint32_t>(slba >> 32);
    cmd.cdw12  = static_cast<uint32_t>(nlb - 1);
    auto sr    = io_submit(cmd);
    if (!sr.ok()) {
        return sr.error();
    }
    if (sr.value() != 0) {
        cinux::lib::kprintf(
            "[NVMe] NVM I/O opcode=0x%x nsid=%u slba=%llu nlb=%u failed status=0x%x\n", opcode,
            nsid, static_cast<unsigned long long>(slba), nlb, sr.value());
        return cinux::lib::Error::IOError;
    }
    return {};
}

cinux::lib::ErrorOr<void> NvmeController::read_blocks(uint32_t nsid, uint64_t slba, uint16_t nlb,
                                                      cinux::drivers::dma::DmaBuffer& buf) {
    return nvm_io(0x02, nsid, slba, nlb, buf);
}

cinux::lib::ErrorOr<void> NvmeController::write_blocks(uint32_t nsid, uint64_t slba, uint16_t nlb,
                                                       cinux::drivers::dma::DmaBuffer& buf) {
    return nvm_io(0x01, nsid, slba, nlb, buf);
}

}  // namespace cinux::drivers::nvme

namespace cinux::arch {
struct InterruptFrame;  // fwd decl for the ISR C handler signature
}

volatile uint64_t g_nvme_irq_count = 0;
extern "C" void nvme_irq_handler(cinux::arch::InterruptFrame* /*frame*/) {
    g_nvme_irq_count = g_nvme_irq_count + 1;  // EOI by ISR_IRQ stub
}
