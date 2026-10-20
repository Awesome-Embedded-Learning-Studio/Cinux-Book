/**
 * @file    devfs.hpp
 * @brief   The device file system: kernel-registered devices behind names.
 *
 * DevFs satisfies FileSystem with plain methods, like RamFs one station
 * back — but the nodes are not bytes on any medium. Each name in the flat
 * root namespace maps to a device the kernel itself registered: reading
 * and writing a device node forwards to that device's two functions, and
 * the offset a regular file would honor means nothing here. The table is
 * born with the two pure-computation devices, null and zero; everything
 * that touches real hardware — the console today — is registered from a
 * wiring file one step below, so this class stays free of kernel I/O and
 * a host test can drive it whole. Creation calls are refused: the device
 * population is a kernel decision, not a file-system one.
 *
 * @author  Charliechen114514
 * @date    2026-10-19
 * @version 0.1
 * @since   0.5.0
 * @ingroup kernel_fs
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <string_view>

#include "cinux/result.hpp"
#include "kernel/fs/fs_config.hpp"
#include "kernel/fs/vfs.hpp"

namespace cinux::fs {

class DevFs;

/**
 * @brief   How many devices the table holds at once.
 * @since   0.5.0
 * @ingroup kernel_fs
 */
inline constexpr unsigned long kDevfsDeviceMax = 8;

/**
 * @brief         Packs a major:minor pair into one device identity word.
 *
 * @param[in]     major   Device class, the high part.
 * @param[in]     minor   Instance inside the class, the low part.
 * @return        The packed identity, Linux-compatible at eight bits of
 *                minor and the rest major.
 * @since         0.5.0
 * @ingroup       kernel_fs
 */
[[nodiscard]] constexpr unsigned long MakeDeviceId(unsigned long major, unsigned long minor) {
    return (major << 8U) | minor;
}

/// Reads count bytes into sink on behalf of one device.
using DeviceRead = base::Result<unsigned long> (*)(void* context, void* sink, unsigned long count);

/// Writes count bytes from source on behalf of one device.
using DeviceWrite = base::Result<unsigned long> (*)(void* context, const void* source,
                                                    unsigned long count);

/// Whether the device could hand a reader bytes right now.
using DeviceHasInput = bool (*)(void* context);

/// One device question, answered with zero or a negative errno.
using DeviceIoctl = long long (*)(void* context, unsigned long long word,
                                  unsigned long long argument);

/**
 * @brief   The faces one device offers behind its name.
 * @note    The two directions are required; a device that cannot answer
 *          one returns its refusing result from its own function — null
 *          answers a clean zero from read — so the table never carries
 *          null function pointers there. Readiness, questions and
 *          retirement are optional: a null has_input reads as
 *          always-ready, a null ioctl as not-a-terminal, a null
 *          release as nothing to give back. The context word rides
 *          along to every seat, the pair-shape FsOps uses one floor up.
 * @since   0.5.0
 * @ingroup kernel_fs
 */
struct DeviceOps {
    DeviceRead     read;       ///< Pulls bytes out of the device.
    DeviceWrite    write;      ///< Pushes bytes into the device.
    DeviceHasInput has_input;  ///< Optional: can a reader progress now.
    DeviceIoctl    ioctl;      ///< Optional: answer one device question.
    /// Optional: the last descriptor on this device has closed.
    void (*release)(void* context);
    void* context;  ///< The word every seat receives back.
};

/**
 * @brief   Seats the two pure-computation devices, null and zero.
 *
 * @param[in,out] devfs   The table receiving them.
 * @return        None
 * @note          Bring-up wiring: a freshly built DevFs is an empty
 *                directory, and the population is a kernel decision
 *                expressed as registrations. Null takes 1:3, zero 1:5,
 *                the Linux pairings.
 * @since         0.5.0
 * @ingroup       kernel_fs
 */
void RegisterBuiltinDevices(DevFs& devfs);

/**
 * @brief   Memory-backed directory of kernel-registered devices.
 * @note    A fresh instance is an empty root at inode one; every device
 *          arrives by registration and takes the next table row and
 *          inode number. Nodes are table rows, so handles stay valid
 *          for the instance's whole life and retain/release are
 *          no-ops. Registration belongs to bring-up, single-threaded,
 *          before the mount goes live; lookups afterwards only read.
 * @since   0.5.0
 * @ingroup kernel_fs
 */
class DevFs {
public:
    DevFs() = default;

    DevFs(const DevFs&)            = delete;
    DevFs& operator=(const DevFs&) = delete;

    /**
     * @brief         Seats one device under a name in the flat namespace.
     *
     * @param[in]     name   Leaf name, no slash, under kFsNameMax.
     * @param[in]     rdev   The device identity stat reports.
     * @param[in]     ops    The device's faces.
     * @return        Success, or kInvalidArgument (shape, over-long),
     *                kAlreadyExists (name seated twice), kOutOfMemory
     *                (table full).
     * @since         0.5.0
     * @ingroup       kernel_fs
     */
    [[nodiscard]] base::Result<void> register_device(std::string_view name, unsigned long rdev,
                                                     const DeviceOps& ops);

    /**
     * @brief         Installs the resolver for names beyond the table.
     *
     * @param[in]     resolve   The resolver, or nullptr to uninstall.
     * @return        None
     * @note          Lookup consults the seated table first; a miss then
     *                asks the resolver, which answers whole FsNodes from
     *                its own namespace (the PTY pairs' /dev/pts seats).
     *                Bring-up wiring, before the mount goes live.
     * @since         0.5.0
     * @ingroup       kernel_fs
     */
    void set_dynamic_lookup(base::Result<FsNode> (*resolve)(std::string_view path));

    /**
     * @brief         Installs the open-time seat exchanger.
     *
     * @param[in]     adopt   The exchanger, or nullptr to uninstall.
     * @return        None
     * @note          Every open passes its node through the exchanger,
     *                which answers the same node back except for a
     *                cloning device, whose open mints a fresh pair and
     *                answers its master. Bring-up wiring, before the
     *                mount goes live.
     * @since         0.5.0
     * @ingroup       kernel_fs
     */
    void set_open_adopter(base::Result<FsNode> (*adopt)(FsNode node, std::string_view path));

    /**
     * @brief         Finds a node by mount-relative path.
     *
     * @param[in]     path   Empty addresses the root; one name addresses
     *                      a device.
     * @return        The node, or kNotFound (no such name, or a path
     *                      trying to walk through the flat namespace).
     * @since         0.5.0
     * @ingroup       kernel_fs
     */
    [[nodiscard]] base::Result<FsNode> lookup(std::string_view path);

    /**
     * @brief         Refused: the population is kernel-registered.
     *
     * @param[in]     path   Where a file was asked to appear.
     * @return        kNotSupported.
     * @since         0.5.0
     * @ingroup       kernel_fs
     */
    [[nodiscard]] base::Result<FsNode> make_file(std::string_view path);

    /**
     * @brief         Refused: the population is kernel-registered.
     *
     * @param[in]     path   Where a directory was asked to appear.
     * @return        kNotSupported.
     * @since         0.5.0
     * @ingroup       kernel_fs
     */
    [[nodiscard]] base::Result<FsNode> make_dir(std::string_view path);

    /**
     * @brief         Refused: the population is kernel-registered.
     *
     * @param[in]     path   Node asked to disappear.
     * @return        kNotSupported.
     * @since         0.5.0
     * @ingroup       kernel_fs
     */
    [[nodiscard]] base::Result<void> unlink(std::string_view path);

    /**
     * @brief         Reads from the device behind a node.
     *
     * @param[in]     node    Handle from lookup.
     * @param[in]     offset  Ignored; devices have no cursor.
     * @param[out]    sink    Destination buffer.
     * @param[in]     count   Bytes wanted.
     * @return        Bytes the device answered, or kIsADirectory when
     *                node is the root.
     * @since         0.5.0
     * @ingroup       kernel_fs
     */
    [[nodiscard]] base::Result<unsigned long> read(FsNode node, unsigned long offset, void* sink,
                                                   unsigned long count) const;

    /**
     * @brief         Writes to the device behind a node.
     *
     * @param[in]     node    Handle from lookup.
     * @param[in]     offset  Ignored; devices have no cursor.
     * @param[in]     source  Bytes the device takes.
     * @param[in]     count   Bytes offered.
     * @return        Bytes the device took, or kIsADirectory when node
     *                is the root.
     * @since         0.5.0
     * @ingroup       kernel_fs
     */
    [[nodiscard]] base::Result<unsigned long> write(FsNode node, unsigned long offset,
                                                    const void* source, unsigned long count) const;

    /**
     * @brief         Hands out one root entry by ordinal.
     *
     * @param[in]     node    Directory handle; devices refuse.
     * @param[in]     index   Zero-based; dot and dot-dot first, then the
     *                        devices in registration order.
     * @param[out]    entry   Name and shape of that entry.
     * @return        True with entry filled; false past the last device.
     *                kNotADirectory when node is a device.
     * @since         0.5.0
     * @ingroup       kernel_fs
     */
    [[nodiscard]] base::Result<bool> read_dir(FsNode node, unsigned long index, FsDirent& entry);

    /**
     * @brief         Reads one node's metadata.
     *
     * @param[in]     node    Handle from lookup.
     * @param[out]    probed  The metadata: directory mode for the root,
     *                        character-device mode and the seated rdev
     *                        for devices; mtime zero, the honest word.
     * @return        Success.
     * @since         0.5.0
     * @ingroup       kernel_fs
     */
    [[nodiscard]] base::Result<void> stat(FsNode node, FsStat& probed);

    /**
     * @brief         Pins a node for an open descriptor.
     *
     * @param[in]     node   Handle from lookup.
     * @return        None
     * @note          No-op: table rows outlive every descriptor.
     * @since         0.5.0
     * @ingroup       kernel_fs
     */
    void retain(FsNode node);

    /**
     * @brief         Drops an open descriptor's pin on a node.
     *
     * @param[in]     node   Handle from lookup.
     * @return        None
     * @note          Table rows outlive every descriptor; a device that
     *                keeps its own seat — a cloned pair, say — hears
     *                the last close through its release face and gives
     *                the seat back itself.
     * @since         0.5.0
     * @ingroup       kernel_fs
     */
    void release(FsNode node) const;

    /**
     * @brief         Whether a reader would make progress on this node now.
     *
     * @param[in]     node   A device node this backend minted.
     * @return        True unless the device's own readiness seat says
     *                otherwise; a device without one is always ready.
     * @since         0.5.0
     * @ingroup       kernel_fs
     */
    [[nodiscard]] bool readable(FsNode node) const;

    /**
     * @brief         Asks one device question.
     *
     * @param[in]     node       A device node this backend minted.
     * @param[in]     word       The question word.
     * @param[in]     argument   The question's argument word.
     * @return        The device's answer, or -ENOTTY when it keeps no
     *                question seat.
     * @since         0.5.0
     * @ingroup       kernel_fs
     */
    [[nodiscard]] long long ioctl(FsNode node, unsigned long long word,
                                  unsigned long long argument) const;

    /**
     * @brief         Replaces a node at open time.
     *
     * @param[in]     node    The node lookup produced.
     * @param[in]     path    The mount-relative path being opened.
     * @return        The node to seat in the descriptor table: the same
     *                node unless the dynamic resolver mints a fresh one
     *                for a cloning device.
     * @since         0.5.0
     * @ingroup       kernel_fs
     */
    [[nodiscard]] base::Result<FsNode> adopt_open(FsNode node, std::string_view path);

    /**
     * @brief   The faces a node carries, whatever seated it.
     * @param[in] node   A node this backend minted: from the table's
     *            rows or the dynamic resolver's own namespace.
     * @return  The device's ops table.
     * @since   0.5.0
     * @ingroup kernel_fs
     */
    [[nodiscard]] const DeviceOps* ops_of(FsNode node) const;

private:
    /**
     * @brief   One seated device: name, identity, and its faces.
     * @since   0.5.0
     * @ingroup kernel_fs
     */
    struct DeviceNode {
        char             name[kFsNameMax] = {};
        unsigned long    rdev             = 0;
        const DeviceOps* ops              = nullptr;
    };

    DeviceNode    devices_[kDevfsDeviceMax] = {};
    unsigned long count_                    = 0;

    base::Result<FsNode> (*dynamic_)(std::string_view path) = nullptr;

    base::Result<FsNode> (*adopter_)(FsNode node, std::string_view path) = nullptr;
};

}  // namespace cinux::fs
