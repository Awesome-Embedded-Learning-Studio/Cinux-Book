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
 * @brief Spawn a shell terminal window (Desktop icon action)
 *
 * Creates a centred Terminal window, wires per-terminal stdin/stdout pipes,
 * forks a shell child task running /bin/sh, and adds the window to the WM.
 * Used by the cinux::gui host desktop->spawn callback (cinux::gui §3b adapter).
 */
void create_shell_terminal();

}  // namespace cinux::gui
