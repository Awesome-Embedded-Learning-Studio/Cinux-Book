/**
 * @file kernel/drivers/block_device.hpp
 * @brief IBlockDevice -- minimal synchronous block device abstraction
 *
 * IBlockDevice decouples filesystems (ext2) and future drivers from any one
 * storage controller.  A consumer speaks in device blocks: read_blocks /
 * write_blocks transfer [block, block+count) device-blocks between the device
 * and a caller-supplied buffer.  The implementation owns whatever DMA plumbing
 * it needs (the AHCI adapter, batch 2, uses the M3 DmaPool); the caller never
 * touches bus addresses.
 *
 * Errors.  This interface is purely in-kernel -- it never crosses a syscall
 * trap -- so it reports failure through ErrorOr (Error::IOError on transfer
 * failure) rather than a bool.  Geometry queries (block_count / block_size)
 * cannot fail and return plain integers.
 *
 * Scope (F1-M4): synchronous transfers only.  No request queue, no async I/O,
 * no page cache -- those belong to later milestones.
 *
 * Namespace: cinux::drivers
 */

#pragma once

#include <cinux/expected.hpp>
#include <cstdint>

namespace cinux::drivers {

/**
 * @brief Minimal synchronous block device interface
 *
 * The unit of transfer is the device block (block_size() bytes); for ATA/SATA
 * devices this is one 512-byte sector.  Callers working in larger units (ext2
 * blocks) convert through their own block-to-device-block factor.
 *
 * The @p buf passed to read_blocks / write_blocks is a plain virtual address;
 * the implementation performs any DMA mapping internally and copies between the
 * DMA buffer and @p buf.
 *
 * @see RAMBlockDevice (test stub / ramdisk backing)
 * @see ahci::AHCIBlockDevice (batch 2 -- AHCI adapter)
 */
class IBlockDevice {
public:
    virtual ~IBlockDevice() = default;

    /**
     * @brief Read @p count device-blocks starting at @p block into @p buf
     * @param block Starting device-block number
     * @param count Number of device-blocks to read
     * @param buf   Destination virtual address (count * block_size() bytes)
     * @return Error::IOError on transfer failure
     */
    virtual cinux::lib::ErrorOr<void> read_blocks(uint64_t block, uint64_t count, void* buf) = 0;

    /**
     * @brief Write @p count device-blocks from @p buf starting at @p block
     * @param block Starting device-block number
     * @param count Number of device-blocks to write
     * @param buf   Source virtual address (count * block_size() bytes)
     * @return Error::IOError on transfer failure
     */
    virtual cinux::lib::ErrorOr<void> write_blocks(uint64_t block, uint64_t count,
                                                   const void* buf) = 0;

    /**
     * @brief Flush volatile write cache to media
     *
     * Devices without a write cache (RAM, or early drivers lacking a flush
     * command) succeed as a no-op via this default.
     */
    virtual cinux::lib::ErrorOr<void> flush() { return {}; }

    /** @brief Total number of device-blocks on the device. */
    virtual uint64_t block_count() const = 0;

    /** @brief Size of one device-block in bytes. */
    virtual uint64_t block_size() const = 0;
};

}  // namespace cinux::drivers
