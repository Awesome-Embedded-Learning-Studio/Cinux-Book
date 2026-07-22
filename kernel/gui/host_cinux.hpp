/**
 * @file kernel/gui/host_cinux.hpp
 * @brief Cinux host adapter -- fills the Host table for the in-kernel desktop
 *
 * The Host ABI table (host.hpp) is the ONLY hard seam between cinux::gui core
 * and host. This unit fills that table for the Cinux kernel desktop so cinux::gui
 * core can run here: every callback forwards to an existing in-tree facility.
 *
 *   poll_event  -> Mouse::event_queue()          (serialised to event)
 *   now_ms      -> PIT::get_uptime_ms()
 *   alloc/free  -> kmalloc/kfree
 *   log         -> kvprintf
 *   flush       -> forward dirty rect: staging back buffer -> VBE framebuffer (§4c)
 *   desktop.spawn -> cinux::gui::create_shell_terminal()
 *
 * Swap this fill for an SDL simulator / MCU table fill and the same pump
 * body runs unchanged.
 *
 * Compile condition: CINUX_GUI.
 *
 * Namespace: cinux::gui
 */
#pragma once

#include "third_party/Cinux-GUI/core/host.hpp"

#ifdef __cplusplus

namespace cinux::drivers {
class Framebuffer;
}

namespace cinux::gui {

/**
 * @brief The filled Cinux host descriptor (singleton)
 *
 * Returned by reference so every caller observes the table filled by
 * cinux_host_init(). Reading before init() yields an all-NULL table
 * (safe: pump() NULL-checks every callback it dereferences).
 *
 * @return reference to the static Cinux host descriptor
 */
Host& cinux_host();

/**
 * @brief Fill the Cinux host descriptor (call once after gui_init())
 *
 * Wires every core callback + the Desktop extension and binds the opaque ctx.
 * @p fb is the hardware framebuffer the flush callback forwards dirty rects to
 * (F13 §4c); pass the screen canvas's framebuffer(). Idempotent.
 *
 * @param fb  The VBE framebuffer to forward flushed rects to (may be null,
 *            in which case flush is a no-op)
 */
void cinux_host_init(cinux::drivers::Framebuffer* fb = nullptr);

}  // namespace cinux::gui

#endif /* __cplusplus */
