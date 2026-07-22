/**
 * @file kernel/gui/desktop_launch.cpp
 * @brief GUI userspace launch (cinux::proc::launch_userspace, CINUX_GUI build)
 *
 * CODING-TASTE §14: this is the GUI-side implementation of the single
 * launch_userspace() interface declared in kernel/proc/userspace.hpp.  It starts
 * the desktop (mouse + window manager + PIT tick callback via gui_start) and
 * spawns the gui_worker thread that drives the cinux::gui pump + services the
 * xHCI event ring each frame.  The non-GUI counterpart lives in
 * kernel/proc/shell_launch.cpp; CMake links exactly one (the gui/ subdirectory
 * is added only under if(CINUX_GUI)).
 */

#include <stdint.h>

#include "kernel/drivers/canvas.hpp"
#include "kernel/drivers/usb/usb_init.hpp"
#include "kernel/drivers/video/console.hpp"
#include "kernel/gui/gui_init.hpp"
#include "kernel/gui/host_cinux.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/task_builder.hpp"
#include "kernel/proc/userspace.hpp"
#include "third_party/Cinux-GUI/core/pump.hpp"

namespace {

/// GUI render/input pump loop.  Drains the event queue through the cinux::gui
/// Host ABI table (F13 §3b: input/time/spawn all go via host->core.* /
/// host->desktop->*), then services the xHCI event ring.  Runs for the system
/// lifetime on its own thread.
void gui_worker_thread() {
    cinux::lib::kprintf("[GUI] Worker thread started\n");
    while (true) {
        cinux::gui::pump(&cinux::gui::cinux_host());
        // Service the xHCI event ring each frame (usb::poll_input is a no-op if
        // no controller was enumerated, or if USB is compiled out -- §14 links
        // usb_stub.cpp).  On QEMU under nested-KVM the MSI-X transfer-complete
        // interrupt is not reliably delivered, so polling the ring here is the
        // production event-service path; cheap when the mouse is idle (the
        // dequeue finds the ring empty).
        cinux::drivers::usb::poll_input();
        cinux::proc::Scheduler::yield();
    }
}

}  // namespace

namespace cinux::proc {

void launch_userspace() {
    cinux::lib::kprintf("[INIT] ===== Milestone 035: Multi-Terminal =====\n");

    // Start the GUI: mouse init, desktop icons, PIT tick callback.  Per-terminal
    // stdin/stdout pipes are created on demand inside create_shell_terminal()
    // (desktop Shell icon action) -- no global pipe setup here.
    cinux::gui::gui_start();

    // Launch the GUI worker thread to drive the render/input pump + deferred
    // work (fork/execve) outside of PIT interrupt context.
    auto* gui_task = TaskBuilder().set_entry(gui_worker_thread).set_name("gui_worker").build();
    if (gui_task != nullptr) {
        Scheduler::add_task(gui_task);
        cinux::lib::kprintf("[INIT] GUI worker thread launched\n");
    }
}

void handoff_framebuffer_to_gui(cinux::drivers::Framebuffer& fb, cinux::drivers::PSFFont& font,
                                cinux::drivers::Console& console) {
    // The canvas is static: it must outlive the boot-time fb it wraps and stay
    // live for the desktop's whole lifetime.  gui_init wires the mouse + window
    // manager + renders the desktop.  Detach the text console so routine logs
    // stop overlaying the desktop (still serial + klog; kpanic re-enables).
    static cinux::drivers::Canvas g_canvas;
    g_canvas.init(fb);
    cinux::gui::gui_init(g_canvas, font);
    cinux::lib::kprintf_set_sink_enabled(cinux::drivers::Console::console_sink_adapter, &console,
                                         false);
}

}  // namespace cinux::proc
