/**
 * @file kernel/gui/gui_init.hpp
 * @brief GUI subsystem initialisation interface
 *
 * Provides a clean entry point for the GUI stack: mouse driver,
 * window manager, and PIT tick callback registration.  All GUI
 * setup is encapsulated here so that kernel_main stays free of
 * GUI details.
 *
 * This header is only compiled when CINUX_GUI is defined.
 *
 * Namespace: cinux::gui
 */

#pragma once

namespace cinux::drivers {
class Canvas;
class PSFFont;
}  // namespace cinux::drivers

namespace cinux::gui {

/**
 * @brief Perform one-time GUI initialisation (call once from kernel_main)
 *
 * Sets up the mouse driver, window manager, and renders the demo
 * screen.  Must be called after the Canvas and PSFFont are ready
 * and after PIC IRQ0/IRQ1 are unmasked and interrupts enabled.
 *
 * @param screen  Reference to the initialised screen canvas
 * @param font    Reference to the initialised PSF2 font
 */
void gui_init(cinux::drivers::Canvas& screen, cinux::drivers::PSFFont& font);

/**
 * @brief Register the GUI tick callback on the PIT (call from kernel_init_thread)
 *
 * After calling this, every PIT tick will drain the event queue,
 * dispatch input to the window manager, and composite the frame.
 * Registers desktop icons (Shell, Calculator) on the desktop.
 */
void gui_start();

/**
 * @brief Process deferred GUI work enqueued by the PIT tick callback
 *
 * Drains the pending action queue and executes any heavy operations
 * (e.g. creating a new terminal window via fork/execve) that cannot
 * safely run in interrupt context.  Should be called in a loop by a
 * dedicated kernel worker thread.
 */
void gui_process_pending();

}  // namespace cinux::gui
