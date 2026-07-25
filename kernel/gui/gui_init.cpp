/**
 * @file kernel/gui/gui_init.cpp
 * @brief GUI subsystem initialisation implementation (F13-B)
 *
 * F-GUI-USERSPACE (2026-07-08): the widget tree + GuiCore live in the USERSPACE
 * GUI host now (user/cinux_gui_host), not in any kernel host adapter. What
 * remains here is the input-side wiring: PS/2 mouse + a keyboard listener that
 * dual-writes each KeyEvent into /dev/event0 so the userspace host's poll_event
 * drains both pointer + keyboard through the one device.
 */

#include "gui_init.hpp"

#include "kernel/drivers/input/input_event_device.hpp"  // /dev/event0 push (F-GUI b2)
#include "kernel/drivers/keyboard/keyboard.hpp"
#include "kernel/drivers/mouse/mouse.hpp"
#include "kernel/gui/event.hpp"  // Event / EventType (queue element)
#include "kernel/lib/kprintf.hpp"

namespace cinux::gui {

namespace {

// Key listener registered with the keyboard driver: dual-write each decoded
// KeyEvent to /dev/event0 so the userspace GUI host reads keyboard input
// alongside pointer events. Lives here (not in keyboard.cpp) so the keyboard
// driver has no GUI dependency -- it just calls a registered listener
// (CODING-TASTE §14).
void on_key_event(const cinux::drivers::KeyEvent& ev) {
    cinux::gui::Event gui_ev{};
    gui_ev.type_     = ev.pressed ? cinux::gui::EventType::KeyDown : cinux::gui::EventType::KeyUp;
    gui_ev.key.ascii = ev.ascii;
    gui_ev.key.scancode = ev.scancode;
    gui_ev.key.pressed  = ev.pressed;
    gui_ev.key.shift    = ev.shift;
    gui_ev.key.ctrl     = ev.ctrl;
    gui_ev.key.alt      = ev.alt;
    cinux::drivers::Mouse::event_queue().enqueue(gui_ev);
    // F-GUI-USERSPACE batch 2: also deliver keyboard events to /dev/event0 so a
    // userspace GUI host can read them.  Zero touch to keyboard.cpp (the driver
    // stays GUI-free; this listener is the seam).
    cinux::input::InputEventDevice::instance().push_event(gui_ev);
}

}  // namespace

void gui_start() {
    cinux::lib::kprintf("[GUI] ===== Milestone 033: GUI Desktop (F13-B new core) =====\n");

    // Initialise PS/2 mouse driver. Screen bounds are set later by
    // cinux_host_init (it owns display geometry now).
    cinux::drivers::Mouse::init();
    // Dual-dispatch keys into the unified queue via a listener (the keyboard
    // has no #ifdef CINUX_GUI; it just calls whoever registered -- §14).
    cinux::drivers::Keyboard::register_key_listener(on_key_event);

    cinux::lib::kprintf(
        "[GUI] Mouse + keyboard listener initialised; desktop driven by the "
        "userspace GUI host (/cinux_gui_host).\n");
}

}  // namespace cinux::gui
