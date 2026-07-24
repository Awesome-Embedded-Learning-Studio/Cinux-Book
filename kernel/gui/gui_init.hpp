/**
 * @file kernel/gui/gui_init.hpp
 * @brief GUI subsystem initialisation interface (F13-B)
 *
 * F-GUI-USERSPACE (2026-07-08): the widget tree + GuiCore are built in the
 * USERSPACE GUI host (user/cinux_gui_host), not in any kernel host adapter.
 * gui_start() remains to register the PS/2 mouse + keyboard listener that
 * dual-writes input to /dev/event0 for the host to read.
 *
 * This header is only compiled when CINUX_GUI is defined.
 *
 * Namespace: cinux::gui
 */

#pragma once

namespace cinux::gui {

/**
 * @brief Register the PS/2 mouse driver + keyboard listener (call from
 *        kernel_init_thread via launch_userspace)
 *
 * After this, every decoded key event is dual-written to /dev/event0 so the
 * userspace GUI host's poll_event drains both pointer + keyboard. The widget
 * tree + GuiCore are built by the host itself over /dev/fb0 + /dev/event0.
 */
void gui_start();

}  // namespace cinux::gui
