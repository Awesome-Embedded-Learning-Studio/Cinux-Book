/**
 * @file kernel/drivers/mouse/mouse.hpp
 * @brief PS/2 mouse driver (IRQ12)
 *
 * Provides interrupt-driven mouse input via the PS/2 controller.
 * The driver initialises the auxiliary device port, configures
 * IRQ12, and parses the standard 3-byte mouse packet format.
 *
 * The poll() interface returns MouseEvent structs containing the
 * absolute cursor position (clamped to screen bounds) and button
 * state.  Internally the driver also pushes events into the
 * GUI EventQueue so the window manager can consume them.
 *
 * This driver is only compiled when CINUX_GUI is defined.
 *
 * Dependencies:
 *   - PIC must be initialised and IRQ12 unmasked after init()
 *   - IDT must have vector 0x2C (IRQ12) registered
 *   - irq_handlers.cpp must route irq12_stub to mouse_irq12_handler
 *
 * Namespace: cinux::drivers
 */

#pragma once

#include <stdint.h>

#include "kernel/gui/event.hpp"

// Forward declaration -- InterruptFrame is defined in idt.hpp
namespace cinux::arch {
struct InterruptFrame;
}  // namespace cinux::arch

namespace cinux::drivers {

// ============================================================
// Mouse class
// ============================================================

/**
 * @brief PS/2 mouse driver with IRQ12 interrupt handling
 *
 * All methods are static because there is exactly one PS/2 mouse
 * in the system.  The IRQ12 handler accumulates 3-byte packets,
 * updates the internal cursor position, and enqueues MouseEvent
 * structs into the GUI EventQueue.
 */
class Mouse {
public:
    /**
     * @brief Initialise the PS/2 mouse via the 8042 controller
     *
     * Sequence:
     *   1. Send CMD 0xA8 -- enable auxiliary device (mouse port)
     *   2. Read config (CMD 0x20), set bit 1 (IRQ12 enable), write back (CMD 0x60)
     *   3. Send CMD 0xD4 + 0xF4 -- activate the mouse (enable streaming)
     *
     * @note Caller must still call PIC::unmask(12) after this.
     */
    static void init();

    /**
     * @brief IRQ12 interrupt handler (called from ISR stub)
     *
     * Reads one byte from port 0x60 and accumulates it into a
     * 3-byte packet buffer.  Once a complete packet is received,
     * it is decoded into a MouseEvent and enqueued into the
     * global EventQueue.  Sends EOI to the PIC before returning.
     *
     * @param frame  Interrupt stack frame (unused)
     */
    static void irq12_handler(cinux::arch::InterruptFrame* frame);

    /**
     * @brief Dequeue a mouse event from the global EventQueue
     *
     * @param out  Reference to a MouseEvent to fill
     * @return     true if a mouse event was dequeued, false if empty
     */
    static bool poll(cinux::gui::MouseEvent& out);

    /**
     * @brief Get the current absolute cursor X position
     * @return Cursor X in pixels (0 .. screen_width - 1)
     */
    static int32_t x();

    /**
     * @brief Get the current absolute cursor Y position
     * @return Cursor Y in pixels (0 .. screen_height - 1)
     */
    static int32_t y();

    /**
     * @brief Set the screen dimensions used for cursor clamping
     *
     * @param width   Screen width in pixels
     * @param height  Screen height in pixels
     */
    static void set_screen_bounds(uint32_t width, uint32_t height);

    /**
     * @brief Get a reference to the global GUI event queue
     *
     * The window manager polls this queue for all input events
     * (mouse and keyboard).
     *
     * @return Reference to the shared EventQueue
     */
    static cinux::gui::EventQueue& event_queue();

    // ---- USB HID input (Batch 4B) ----

    /**
     * @brief Inject a decoded HID boot-mouse report into the event queue.
     *
     * Updates the absolute cursor (clamped) + button state + enqueues move /
     * down / up events, exactly like the PS/2 path but with the HID Y axis
     * convention (dy NOT inverted: HID Y+ == screen DOWN).  Used by the xHCI
     * HID worker so a USB mouse drives the cursor.
     *
     * @param dx       X displacement (signed)
     * @param dy       Y displacement (signed, screen-DOWN positive)
     * @param buttons  bit0=left, bit1=right, bit2=middle
     */
    static void inject_usb_motion(int8_t dx, int8_t dy, uint8_t buttons);

    /**
     * @brief Inject an absolute pointing-device report (USB tablet).
     *
     * Sets the cursor directly to the screen pixel mapped from the tablet's
     * 0..32767 logical X/Y range, so the cursor tracks the host cursor exactly
     * (a relative mouse drifts at the screen edge -- two-cursor skew).  Same
     * button bit layout as the boot mouse (0=left, 1=right, 2=middle).
     *
     * @param tx       Tablet X (0..32767 logical range)
     * @param ty       Tablet Y (0..32767 logical range)
     * @param buttons  bit0=left, bit1=right, bit2=middle
     */
    static void inject_usb_absolute(uint16_t tx, uint16_t ty, uint8_t buttons);

    /**
     * @brief Select the active input source.
     *
     * When USB is primary, PS/2 packet bytes are ignored (the PS/2 path does
     * not feed the event queue), preserving the single-producer invariant of
     * the SPSC queue.  PS/2 remains the default until a USB mouse is booted.
     *
     * @param primary  true to route input through the USB (xHCI) path
     */
    static void set_usb_primary(bool primary);

private:
    /**
     * @brief Apply a movement + button state to the cursor + event queue.
     *
     * Shared tail of the PS/2 (decode_packet) and USB (inject_usb_motion)
     * paths: updates mouse_x_/mouse_y_ by @p dy_screen (already in screen
     * space -- PS/2 passes the negated Y, USB passes it as-is), clamps to the
     * screen bounds, detects button edges, and enqueues the events.
     */
    static void apply_motion(int32_t dx, int32_t dy_screen, uint8_t new_buttons);

    /**
     * @brief Set the cursor to an absolute screen position + enqueue events.
     *
     * Shared core used by apply_motion (relative: new = old + delta) and
     * inject_usb_absolute (tablet: new = mapped tablet coord).  Sets mouse_x_/y_
     * to the clamped (@p new_x, @p new_y), then enqueues move/down/up events
     * whose reported delta is (@p ev_dx, @p ev_dy) -- the relative path passes
     * its input delta, the absolute path passes the actual move.
     */
    static void update_absolute(int32_t new_x, int32_t new_y, int32_t ev_dx, int32_t ev_dy,
                                uint8_t new_buttons);

    /**
     * @brief Accumulate a byte from the mouse data port
     *
     * Builds up a 3-byte packet (byte0 = buttons + flags,
     * byte1 = dx, byte2 = dy).  When a full packet is assembled,
     * calls decode_packet().  No-op when USB is the primary input source.
     *
     * @param byte  One byte read from port 0x60
     */
    static void process_byte(uint8_t byte);

    /**
     * @brief Decode a complete 3-byte mouse packet
     *
     * Extracts dx, dy, button state, updates the absolute cursor
     * position (clamped to screen bounds), and enqueues the
     * appropriate MouseEvent(s) into the EventQueue.
     *
     * @param b0  Packet byte 0 (buttons + overflow flags)
     * @param b1  Packet byte 1 (delta X, 9-bit signed)
     * @param b2  Packet byte 2 (delta Y, 9-bit signed)
     */
    static void decode_packet(uint8_t b0, uint8_t b1, uint8_t b2);

    // Packet assembly state
    static constexpr uint8_t PACKET_SIZE = 3;

    static uint8_t packet_[PACKET_SIZE];
    static uint8_t packet_idx_;

    // Cursor state
    static int32_t mouse_x_;
    static int32_t mouse_y_;
    static uint8_t buttons_;

    // Screen bounds for clamping
    static uint32_t screen_width_;
    static uint32_t screen_height_;

    // Previous button state for edge detection (press/release)
    static uint8_t prev_buttons_;

    // Input-source selection: when true, PS/2 bytes are ignored (USB owns the queue).
    static bool usb_primary_;

    // Global event queue
    static cinux::gui::EventQueue g_event_queue_;
};

}  // namespace cinux::drivers
