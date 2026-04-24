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

namespace cinux::ipc { class Pipe; }

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
 * @brief Store the shell pipe pointers for later terminal creation
 *
 * Must be called before gui_start().  The stored pointers are used
 * when the user clicks the Shell desktop icon to create a Terminal
 * window with connected stdin/stdout pipes.
 *
 * @param stdin_pipe   Pointer to the stdin Pipe (Terminal writes to it)
 * @param stdout_pipe  Pointer to the stdout Pipe (Terminal reads from it)
 */
void set_shell_pipes(cinux::ipc::Pipe* stdin_pipe, cinux::ipc::Pipe* stdout_pipe);

/**
 * @brief Register the GUI tick callback on the PIT (call from kernel_init_thread)
 *
 * After calling this, every PIT tick will drain the event queue,
 * dispatch input to the window manager, and composite the frame.
 * Registers desktop icons (Shell, Calculator) on the desktop.
 */
void gui_start();

}  // namespace cinux::gui
