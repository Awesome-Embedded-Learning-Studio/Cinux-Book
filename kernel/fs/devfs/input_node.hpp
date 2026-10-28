/**
 * @file    input_node.hpp
 * @brief   Wiring the input devices into devfs: the event nodes.
 *
 * One shared face behind two names: /dev/input/event0 is the keyboard's
 * stream, /dev/input/event1 the pointer's. The face reads whole wire
 * records, answers the readiness question for poll, and carries the
 * grab word to the stream it serves — grabbing the keyboard is what
 * quiets its glyph feed to the console.
 *
 * @author  Charliechen114514
 * @date    2026-10-26
 * @version 0.1
 * @since   0.9.0
 * @ingroup kernel_fs
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "kernel/driver/input/event_stream.hpp"

namespace cinux::fs {

/**
 * @brief         Seats /dev/input/event0 and /dev/input/event1.
 *
 * @param[in]     keyboard  The keyboard's event stream, event0's source.
 * @param[in]     pointer   The pointer's event stream, event1's source.
 * @return  None
 * @note    Bring-up wiring, single-threaded, before the devfs mount
 *          goes live. The streams arrive as arguments so the seat can
 *          be driven by host tests without the device singletons; a
 *          seat with no device behind it answers no input, the honest
 *          empty node.
 * @since   0.9.0
 * @ingroup kernel_fs
 */
void RegisterInputDevices(driver::EventStream& keyboard, driver::EventStream& pointer);

}  // namespace cinux::fs
