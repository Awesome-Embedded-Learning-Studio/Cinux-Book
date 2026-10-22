/**
 * @file    device.hpp
 * @brief   The device model: every device the kernel owns, on one tree.
 *
 * Drivers bring devices up; this model is where a device becomes a fact
 * the rest of the kernel can see. Each entry names one device, files it
 * under one of the three production classes — character, block, network
 * — and records which bus and which driver claim it. A character device
 * is seated into devfs as part of the same breath, so a driver's one
 * registration answers both "does the kernel know this device" and
 * "where does the file world find it". Block devices belong to the
 * block layer, not to a file seat, and network devices are packets, not
 * files: both register here without touching devfs, and that difference
 * is itself the lesson. The registry is bring-up wiring: seats arrive
 * single-threaded before the world goes live, and later traffic only
 * reads.
 *
 * @author  Charliechen114514
 * @date    2026-10-21
 * @version 0.1
 * @since   0.6.0
 * @ingroup kernel_driver
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <string_view>

#include "cinux/result.hpp"
#include "cinux/singleton.hpp"
#include "kernel/driver/device/device_config.hpp"
#include "kernel/fs/fs_config.hpp"

namespace cinux::fs {
class DevFs;
struct DeviceOps;
}  // namespace cinux::fs

namespace cinux::driver {

/**
 * @brief   The three production classes a device files itself under.
 * @since   0.6.0
 * @ingroup kernel_driver
 */
enum class DeviceClass : unsigned char {
    kCharacter,  ///< A byte stream behind a devfs name.
    kBlock,      ///< Sectors behind the block layer, never a file seat.
    kNetwork,    ///< Frames behind a network face, never a file seat.
};

/**
 * @brief   One device's story, as the observation seats tell it.
 * @note    Names copy whole; the facts travel by value so no record can
 *          be read while the registry moves on.
 * @since   0.6.0
 * @ingroup kernel_driver
 */
struct DeviceFacts {
    char          name[fs::kFsNameMax]         = {};
    DeviceClass   klass                        = DeviceClass::kCharacter;
    unsigned long number                       = 0;
    char          bus[kDeviceLabelMax]         = {};
    char          driver_name[kDeviceLabelMax] = {};
};

/**
 * @brief   One device asking the model for its seat, in one breath.
 * @note    The ask carries everything a seat decides by: the class it
 *          files under, its name, its packed identity word, its faces
 *          when it is a character device, and the bus and driver names
 *          claiming it. Faces stay a borrowed pointer: the asker's ops
 *          table outlives bring-up, the same lifetime devfs seats have
 *          always assumed.
 * @since   0.6.0
 * @ingroup kernel_driver
 */
struct DeviceSeat {
    DeviceClass          klass = DeviceClass::kCharacter;  ///< The class it files under.
    std::string_view     name;                             ///< Its name, its devfs leaf too.
    unsigned long        number = 0;                       ///< Packed identity, zero for network.
    const fs::DeviceOps* ops    = nullptr;                 ///< Faces; required for character.
    std::string_view     bus;                              ///< The bus claiming the device.
    std::string_view     driver_name;                      ///< The driver claiming the device.
};

/**
 * @brief   The one registry every device joins.
 * @note    Bring-up wiring: bind() names the devfs character seats land
 *          in, then drivers seat their devices once, single-threaded,
 *          before the world goes live. Lookup and enumeration only read
 *          afterwards. Device names are unique across the whole model:
 *          a name is a device's identity in every observation seat.
 * @since   0.6.0
 * @ingroup kernel_driver
 */
class DeviceModel : public cinux::base::Singleton<DeviceModel> {
    friend class cinux::base::Singleton<DeviceModel>;

public:
    /**
     * @brief         Names the devfs that character seats land in.
     * @param[in,out] devfs   The device file system mount binds to.
     * @return        None
     * @note          Bring-up wiring, once, before any character seat.
     * @since         0.6.0
     * @ingroup       kernel_driver
     */
    void bind(fs::DevFs& devfs);

    /**
     * @brief         The devfs this model seats character devices in.
     * @return        The bound device file system.
     * @note          For wiring that needs devfs's own services, like
     *                the PTY pair's dynamic seats; refuses with a check
     *                when nothing was bound.
     * @since         0.6.0
     * @ingroup       kernel_driver
     */
    [[nodiscard]] fs::DevFs& bound_devfs() const;

    /**
     * @brief         Seats one device, whichever class it carries.
     * @param[in]     ask   The device's whole ask: class, name, identity,
     *                      faces, and its bus and driver claimants.
     * @return        Success, or kInvalidArgument (shape, or a character
     *                device without faces), kAlreadyExists (name seated
     *                twice), kOutOfMemory (either table full), or the
     *                devfs seat's own refusal.
     * @note          A character device must carry faces and lands in
     *                the bound devfs as part of the same breath; when the
     *                file seat refuses, the model records nothing, so the
     *                registry never holds a device the file world lacks.
     *                A block device serves the block layer and a network
     *                device moves frames, so neither takes a devfs seat,
     *                and a network device carries no number at all.
     * @since         0.6.0
     * @ingroup       kernel_driver
     */
    [[nodiscard]] base::Result<void> seat(const DeviceSeat& ask);

    /**
     * @brief         Hands one device's facts out by ordinal.
     * @param[in]     ordinal   Zero-based, in seating order.
     * @param[out]    into      The device's facts.
     * @return        True with facts filled; false past the last device.
     * @since         0.6.0
     * @ingroup       kernel_driver
     */
    [[nodiscard]] bool nth_device(unsigned long ordinal, DeviceFacts& into) const;

    /**
     * @brief         How many devices the model tracks.
     * @return        The seated device count.
     * @since         0.6.0
     * @ingroup       kernel_driver
     */
    [[nodiscard]] unsigned long device_count() const;

private:
    DeviceModel() = default;

    [[nodiscard]] base::Result<void> check_seat(const DeviceSeat& ask) const;

    [[nodiscard]] base::Result<void> seat_record(const DeviceSeat& ask);

    DeviceFacts   records_[kDeviceModelMax] = {};
    unsigned long count_                    = 0;
    fs::DevFs*    devfs_                    = nullptr;
};

}  // namespace cinux::driver
