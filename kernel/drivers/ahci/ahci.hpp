/**
 * @file kernel/drivers/ahci/ahci.hpp
 * @brief AHCI (SATA) host bus adapter driver
 *
 * Provides an AHCI class that initialises the HBA controller found
 * via PCI enumeration, maps BAR5 MMIO, sets up command lists and
 * FIS buffers for each active port, and offers sector-level
 * read/write operations.
 *
 * Usage:
 *   PCI pci;
 *   pci.init();
 *   PCIDevice ahci_dev;
 *   pci.find_ahci(ahci_dev);
 *
 *   AHCI ahci;
 *   ahci.init(ahci_dev);
 *
 *   uint8_t buf[512];
 *   ahci.read(0, 0, 1, buf);
 *
 * Namespace: cinux::drivers::ahci
 */

#pragma once

#include <stdint.h>

#include <cinux/expected.hpp>

#include "ahci_config.hpp"
#include "kernel/drivers/dma/dma_buffer.hpp"
#include "kernel/drivers/pci/pci.hpp"

namespace cinux::drivers::ahci {

// ============================================================
// AHCI Driver Class
// ============================================================

/**
 * @brief AHCI SATA host bus adapter driver
 *
 * Manages the AHCI controller lifecycle: maps MMIO, probes ports,
 * allocates command list and FIS buffers, and provides blocking
 * sector read/write via DMA.
 */
class AHCI {
public:
    static AHCI& instance();
    static void  set_instance(AHCI* ahci);
    /// @brief True once an AHCI controller was registered via set_instance().
    /// Call BEFORE instance(): instance() dereferences the singleton and #GPs
    /// if no controller was ever registered (e.g. a boot with only NVMe + IDE
    /// and no AHCI device).  Absent controllers must skip the AHCI fallback.
    static bool  is_present() { return s_instance_ != nullptr; }
    /**
     * @brief Initialise the AHCI controller from a PCI device descriptor
     *
     * Performs the following sequence:
     *   1. Map BAR5 into kernel virtual address space (via VMM)
     *   2. Enable AHCI mode (GHC.AE bit)
     *   3. Reset the HBA (GHC.HR bit)
     *   4. Probe each port in the PI bitmap
     *   5. For each active port with a device detected:
     *      - Allocate command list (32 x 32 B, physically contiguous)
     *      - Allocate FIS receive buffer (256 B)
     *      - Configure port registers (CLB, FB, CMD)
     *
     * @param dev  PCI device descriptor for the AHCI HBA (from pci_find_ahci)
     */
    void         init(const pci::PCIDevice& dev);

    /**
     * @brief Read sectors from a SATA device via DMA
     *
     * Constructs a Register H2D FIS with ATA READ DMA EXT (0x25),
     * sets up the PRDT to point at the caller's buffer, issues the
     * command, and polls the port interrupt status until completion.
     *
     * @param port_index  Port number (0-31)
     * @param lba         Starting Logical Block Address (48-bit)
     * @param count       Number of 512-byte sectors to read
     * @param buf         Physical address of the destination buffer
     *                    (must be physically contiguous and aligned)
     * @return            true on success, false on timeout or error
     */
    bool read(uint8_t port_index, uint64_t lba, uint16_t count, uint64_t buf);

    /**
     * @brief Write sectors to a SATA device via DMA
     *
     * Same as read() but uses ATA WRITE DMA EXT (0x35).
     *
     * @param port_index  Port number (0-31)
     * @param lba         Starting Logical Block Address (48-bit)
     * @param count       Number of 512-byte sectors to write
     * @param buf         Physical address of the source buffer
     * @return            true on success, false on timeout or error
     */
    bool write(uint8_t port_index, uint64_t lba, uint16_t count, uint64_t buf);

    /**
     * @brief Get the pointer to the MMIO-mapped HBA registers
     * @return Pointer to the HBAMem structure, or nullptr if not initialised
     */
    HBAMem* hba_mem() const;

    /**
     * @brief Issue ATA IDENTIFY DEVICE and return the device capacity
     * @return Sector count (max LBA + 1), or Error::IOError / InvalidArgument
     */
    cinux::lib::ErrorOr<uint64_t> identify(uint8_t port_index);

    /**
     * @brief Issue FLUSH CACHE EXT to flush the device write cache
     * @return Error::Ok on success, Error::IOError / InvalidArgument
     */
    cinux::lib::ErrorOr<void> flush(uint8_t port_index);

private:
    /**
     * @brief Map BAR5 into the kernel virtual address space
     *
     * @param bar5_phys  Physical address of BAR5 (from PCI)
     * @return Pointer to the mapped HBAMem, or nullptr on failure
     */
    static HBAMem* map_bar5(uint64_t bar5_phys);

    /**
     * @brief Reset the HBA controller
     *
     * Sets GHC.HR and waits for it to clear (max ~1 s).
     */
    void reset_hba() const;

    /**
     * @brief Configure a single port for command processing
     *
     * Stops the port engine, allocates command list and FIS buffer,
     * writes CLB/FB registers, then restarts the engine.
     *
     * @param port_index  The port number to set up
     */
    void setup_port(uint8_t port_index);

    /**
     * @brief Stop a port's command engine (CR, FR, ST, FRE)
     *
     * @param port  Pointer to the port registers
     */
    static void stop_port(HBAPort* port);

    /**
     * @brief Start a port's command engine (FRE, ST)
     *
     * @param port  Pointer to the port registers
     */
    static void start_port(HBAPort* port);

    /**
     * @brief Issue a command on a port and poll for completion
     *
     * @param port_index  Port number
     * @param slot        Command slot index (0-31)
     * @param command     ATA command byte (e.g. AtaCmd::READ_DMA_EXT)
     * @param lba         Starting LBA
     * @param count       Sector count
     * @param buf_phys    Physical address of the data buffer
     * @return            true if the command completed successfully
     */
    bool execute_command(uint8_t port_index, uint8_t slot, uint8_t command, uint64_t lba,
                         uint16_t count, uint64_t buf_phys);

    /**
     * @brief Build the Command FIS in the command table
     *
     * @param cmd_tbl     Pointer to the command table
     * @param command     ATA command byte
     * @param lba         Starting LBA
     * @param count       Sector count
     */
    static void build_cfis(HBACommandTable* cmd_tbl, uint8_t command, uint64_t lba, uint16_t count);

    /// MMIO base pointer (virtual address of BAR5)
    HBAMem* hba_mem_{};

    static AHCI* s_instance_;

    /// Per-port command list + command table DMA (command list + tables share one page)
    cinux::drivers::dma::DmaBuffer cmd_list_buf_[MAX_PORTS];

    /// Per-port FIS receive buffer DMA
    cinux::drivers::dma::DmaBuffer fis_buf_[MAX_PORTS];
};

}  // namespace cinux::drivers::ahci
