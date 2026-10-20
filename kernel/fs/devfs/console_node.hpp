/**
 * @file    console_node.hpp
 * @brief   Wiring the console into devfs: the one node with real I/O.
 *
 * DevFs itself stays free of kernel I/O so a host test can drive it
 * whole; this is the kernel-only step below. One registration, called
 * from FileWorld::init, seats /dev/console (5:1, the Linux pairing)
 * whose read hands the caller the Tty line discipline's next edited
 * line and whose write pushes bytes to every console sink.
 *
 * @author  Charliechen114514
 * @date    2026-10-19
 * @version 0.1
 * @since   0.5.0
 * @ingroup kernel_fs
 * @copyright Copyright (c) 2026
 */

#pragma once

namespace cinux::fs {

class DevFs;

/**
 * @brief         Seats the console device in a devfs instance.
 *
 * @param[in,out] devfs   The table receiving /dev/console.
 * @return        None
 * @note          Bring-up wiring, single-threaded, before the mount
 *                goes live — same discipline as DevFs::register_device.
 * @since         0.5.0
 * @ingroup       kernel_fs
 */
void RegisterConsoleDevice(DevFs& devfs);

/**
 * @brief         Names the group the console's keys belong to.
 *
 * @param[in]     pgid   The foreground group, the login shell's own.
 * @return        None
 * @note          The boot brings the login shell up under init, not
 *                through a TIOCSCTTY the shell asked for itself, so
 *                bring-up hands the foreground over in the same step
 *                that claims the console for that shell.
 * @since         0.5.0
 * @ingroup       kernel_fs
 */
void AdoptConsoleForeground(unsigned long long pgid);

}  // namespace cinux::fs
