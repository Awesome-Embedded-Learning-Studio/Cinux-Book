/**
 * @file kernel/drivers/block_registry.hpp
 * @brief BlockRegistry -- name -> IBlockDevice table for sys_mount (F6-M1 B1b)
 *
 * Boot registers every available block device (NVMe / AHCI ports / VirtIO-blk)
 * under a Linux-ish name (sda, sdb, ...); sys_mount -t ext2 resolves its source
 * path through DevFs to one of these.  Hobby-scale bdev_map (no major/minor, just
 * a flat name table).
 *
 * Namespace: cinux::drivers
 */

#pragma once

#include <stdint.h>

namespace cinux::drivers {

class IBlockDevice;

class BlockRegistry {
public:
    static constexpr uint32_t MAX_DEVICES = 16;
    static constexpr uint32_t NAME_MAX   = 32;

    /// Register @p dev under @p name.  Returns false on a null device, a
    /// duplicate name, or a full table.
    static bool register_device(const char* name, IBlockDevice* dev);

    /// Look up the device registered under @p name, or nullptr.
    static IBlockDevice* lookup(const char* name);

    /// Number of registered devices.
    static uint32_t count();

    /// Iteration helpers for DevFs boot wiring (register /dev/<name> per entry).
    /// name_at returns nullptr when @p i is out of range.
    static const char*   name_at(uint32_t i);
    static IBlockDevice* device_at(uint32_t i);
};

}  // namespace cinux::drivers
