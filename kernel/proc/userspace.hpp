/**
 * @file kernel/proc/userspace.hpp
 * @brief Userspace launch strategy (GUI desktop vs non-GUI shell)
 *
 * CODING-TASTE §14: kernel_init_thread must not branch on #ifdef over which
 * userspace to bring up.  launch_userspace() is a single interface with two
 * implementations, linked by CMake:
 *   - CINUX_GUI on  -> kernel/gui/desktop_launch.cpp (desktop + gui_worker)
 *   - CINUX_GUI off -> kernel/proc/shell_launch.cpp   (fork + exec /bin/sh)
 * Callers (init.cpp) see one straight line, no #ifdef.
 *
 * Namespace: cinux::proc
 */

#pragma once

namespace cinux::drivers {
class Framebuffer;
class PSFFont;
class Console;
}  // namespace cinux::drivers

namespace cinux::proc {

/// Bring up userspace after the VFS is mounted.  GUI build: start the desktop
/// (mouse + window manager + PIT tick callback) and spawn the gui_worker thread.
/// Non-GUI build: fork and exec /bin/sh on legacy sys_read/sys_write.  Exactly
/// one of the two impl files is linked, selected by CMake (see file header).
void launch_userspace();

/// Hand the framebuffer + text console off to the GUI (call from kernel_main
/// once fb/font/console are ready, before sti).  GUI build: create the canvas,
/// init the window manager, and detach the text console so routine logs stop
/// overlaying the desktop (they still go to serial + the klog ring; kpanic
/// re-enables all sinks so a crash still reaches the screen).  Non-GUI build:
/// no-op stub.  §14: one interface, CMake selects the impl.
void handoff_framebuffer_to_gui(cinux::drivers::Framebuffer& fb, cinux::drivers::PSFFont& font,
                                cinux::drivers::Console& console);

}  // namespace cinux::proc
