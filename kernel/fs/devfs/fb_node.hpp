/**
 * @file    fb_node.hpp
 * @brief   Wiring the framebuffer into devfs: the /dev/fb0 seat.
 *
 * The pixels stay where the boot put them; this node is only the door
 * onto them. Two ioctl words answer the fbdev geometry records, and the
 * mapping seat names the hardware frames behind an offset, which is the
 * whole difference between this device and a byte stream: readers get
 * the frame itself, shared, writable, with no copy in between.
 *
 * @author  Charliechen114514
 * @date    2026-10-26
 * @version 0.1
 * @since   0.9.0
 * @ingroup kernel_fs
 * @copyright Copyright (c) 2026
 */

#pragma once

namespace cinux::fs {

/**
 * @brief         Seats /dev/fb0 and reserves its frames as hardware-owned.
 *
 * @return  None
 * @note    Bring-up wiring, single-threaded, before the devfs mount goes
 *          live. A dead framebuffer (no usable boot geometry) refuses
 *          loudly: a GUI lane that reaches this station with no pixels
 *          behind it has lost its screen before it started.
 * @since   0.9.0
 * @ingroup kernel_fs
 */
void RegisterFramebufferDevice();

}  // namespace cinux::fs
