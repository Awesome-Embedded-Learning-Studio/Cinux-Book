/**
 * @file kernel/drivers/keyboard/keyboard.cpp
 * @brief PS/2 keyboard driver implementation
 *
 * Implements PS/2 controller initialisation, scan code set 1 decoding,
 * modifier tracking, ASCII translation, and a ring-buffer event queue.
 */

#include "keyboard.hpp"
#include "kernel/drivers/ps2/ps2.hpp"  // 8042 controller constants (shared with mouse)

#include <stdint.h>

#include "hid.hpp"  // HID keycode->ASCII tables + modifier bits (USB keyboard)
#include "kernel/arch/x86_64/io.hpp"
#include "kernel/drivers/hpet/hpet.hpp"  // g_hpet.monotonic_ns (USB autorepeat)
#include "kernel/drivers/tty/console_tty.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/proc/sync.hpp"

using cinux::io::io_inb;
using cinux::io::io_outb;
using cinux::io::io_wait;
using cinux::lib::kprintf;

namespace cinux::drivers {

// PS/2 controller constants (Ps2Port / Ps2Cmd / Ps2Status) live in
// kernel/drivers/ps2/ps2.hpp -- shared with mouse.cpp (the 8042 is one
// controller wired to both devices).

// ============================================================
// Scan Code Set 1 Special Keys (internal)
// ============================================================

namespace ScanCode {
constexpr uint8_t LSHIFT   = 0x2A;
constexpr uint8_t RSHIFT   = 0x36;
constexpr uint8_t LCTRL    = 0x1D;
constexpr uint8_t LALT     = 0x38;
constexpr uint8_t CAPS     = 0x3A;
constexpr uint8_t EXTENDED = 0xE0;
}  // namespace ScanCode

// ============================================================
// Ring Buffer Constants (internal)
// ============================================================

static constexpr uint32_t SCAN_TABLE_SIZE = 128;

// ============================================================
// Scan Code Set 1 -> ASCII Lookup Tables (data-driven)
// ============================================================

/**
 * @brief Scan code set 1 to lowercase ASCII translation table
 *
 * Indexed by make code.  0 means non-printable or no mapping.
 */
static constexpr char kScToLower[SCAN_TABLE_SIZE] = {
    0,    27,  '1', '2',  '3',  '4', '5',  '6',  // 0x00-0x07
    '7',  '8', '9', '0',  '-',  '=', '\b', 0,    // 0x08-0x0F
    'q',  'w', 'e', 'r',  't',  'y', 'u',  'i',  // 0x10-0x17
    'o',  'p', '[', ']',  '\n', 0,   'a',  's',  // 0x18-0x1F
    'd',  'f', 'g', 'h',  'j',  'k', 'l',  ';',  // 0x20-0x27
    '\'', '`', 0,   '\\', 'z',  'x', 'c',  'v',  // 0x28-0x2F
    'b',  'n', 'm', ',',  '.',  '/', 0,    '*',  // 0x30-0x37
    0,    ' ', 0,   0,    0,    0,   0,    0,    // 0x38-0x3F
    0,    0,   0,   0,    0,    0,   0,    '7',  // 0x40-0x47
    '8',  '9', '-', '4',  '5',  '6', '+',  '1',  // 0x48-0x4F
    '2',  '3', '0', '.',  0,    0,   0,    0,    // 0x50-0x57
    0,    0,   0,   0,    0,    0,   0,    0,    // 0x58-0x5F
    0,    0,   0,   0,    0,    0,   0,    0,    // 0x60-0x67
    0,    0,   0,   0,    0,    0,   0,    0,    // 0x68-0x6F
    0,    0,   0,   0,    0,    0,   0,    0,    // 0x70-0x77
    0,    0,   0,   0,    0,    0,   0,    0,    // 0x78-0x7F
};

/**
 * @brief Scan code set 1 to uppercase / shifted ASCII translation table
 */
static constexpr char kScToUpper[SCAN_TABLE_SIZE] = {
    0,   27,  '!', '@', '#',  '$', '%',  '^',  // 0x00-0x07
    '&', '*', '(', ')', '_',  '+', '\b', 0,    // 0x08-0x0F
    'Q', 'W', 'E', 'R', 'T',  'Y', 'U',  'I',  // 0x10-0x17
    'O', 'P', '{', '}', '\n', 0,   'A',  'S',  // 0x18-0x1F
    'D', 'F', 'G', 'H', 'J',  'K', 'L',  ':',  // 0x20-0x27
    '"', '~', 0,   '|', 'Z',  'X', 'C',  'V',  // 0x28-0x2F
    'B', 'N', 'M', '<', '>',  '?', 0,    '*',  // 0x30-0x37
    0,   ' ', 0,   0,   0,    0,   0,    0,    // 0x38-0x3F
    0,   0,   0,   0,   0,    0,   0,    '7',  // 0x40-0x47
    '8', '9', '-', '4', '5',  '6', '+',  '1',  // 0x48-0x4F
    '2', '3', '0', '.', 0,    0,   0,    0,    // 0x50-0x57
    0,   0,   0,   0,   0,    0,   0,    0,    // 0x58-0x5F
    0,   0,   0,   0,   0,    0,   0,    0,    // 0x60-0x67
    0,   0,   0,   0,   0,    0,   0,    0,    // 0x68-0x6F
    0,   0,   0,   0,   0,    0,   0,    0,    // 0x70-0x77
    0,   0,   0,   0,   0,    0,   0,    0,    // 0x78-0x7F
};

// ============================================================
// Static storage
// ============================================================

cinux::lib::RingBuffer<KeyEvent, Keyboard::KEY_QUEUE_SIZE> Keyboard::buf_;

bool Keyboard::shift_held_ = false;
bool Keyboard::ctrl_held_  = false;
bool Keyboard::alt_held_   = false;

bool                  Keyboard::usb_primary_      = false;
uint8_t               Keyboard::usb_prev_keys_[6] = {};
Keyboard::KeyListener Keyboard::key_listener_     = nullptr;
uint8_t               Keyboard::usb_repeat_key_      = 0;
uint64_t              Keyboard::usb_repeat_deadline_ = 0;

// ============================================================
// Internal helpers
// ============================================================

namespace {

/**
 * @brief Wait until the PS/2 controller input buffer is empty
 *
 * Spins until bit 1 (INPUT_FULL) of the status register is clear.
 */
void wait_input_empty() {
    uint32_t timeout = 100000;
    while ((io_inb(Ps2Port::STATUS) & Ps2Status::INPUT_FULL) != 0) {
        if (--timeout == 0) {
            return;
        }
        __asm__ volatile("pause");
    }
}

/**
 * @brief Send a command byte to the PS/2 controller
 *
 * Waits for input buffer to be empty, then writes to command port 0x64.
 *
 * @param cmd  The command byte to send
 */
void send_command(uint8_t cmd) {
    wait_input_empty();
    io_outb(Ps2Port::COMMAND, cmd);
}

}  // anonymous namespace

// ============================================================
// Keyboard::init() -- PS/2 controller initialisation sequence
// ============================================================

void Keyboard::init() {
    // Step 1: Disable both PS/2 device ports so no data arrives
    send_command(Ps2Cmd::DISABLE_PORT1);
    send_command(Ps2Cmd::DISABLE_PORT2);

    // Step 2: Flush the output buffer (discard any stale data)
    while ((io_inb(Ps2Port::STATUS) & Ps2Status::OUTPUT_FULL) != 0) {
        io_inb(Ps2Port::DATA);
    }

    // Step 3: Read the current controller configuration byte
    send_command(Ps2Cmd::READ_CONFIG);
    wait_input_empty();
    uint8_t config = io_inb(Ps2Port::DATA);

    // Step 4: Modify config: enable IRQ1 (bit 0), disable IRQ12 (bit 1 clear),
    //         disable mouse translation (bit 6 clear)
    config |= 0x01;   // Enable keyboard IRQ (IRQ1)
    config &= ~0x02;  // Disable mouse IRQ (IRQ12)
    config |= 0x40;   // Enable scan code set 2 → set 1 translation

    send_command(Ps2Cmd::WRITE_CONFIG);
    wait_input_empty();
    io_outb(Ps2Port::DATA, config);

    // Step 5: Controller self-test (command 0xAA, expect 0x55)
    send_command(Ps2Cmd::SELF_TEST);
    wait_input_empty();
    uint8_t result = io_inb(Ps2Port::DATA);

    if (result == 0x55) {
        kprintf("[KBD] PS/2 controller self-test passed.\n");
    } else {
        kprintf("[KBD] PS/2 controller self-test FAILED (got 0x%02X, expected 0x55)\n", result);
    }

    // Step 6: Re-enable the first PS/2 port (keyboard)
    send_command(Ps2Cmd::ENABLE_PORT1);

    // Step 7: Reset internal state
    buf_.clear();
    shift_held_ = false;
    ctrl_held_  = false;
    alt_held_   = false;

    kprintf("[KBD] Keyboard driver initialised.\n");
}

// ============================================================
// Keyboard::irq1_handler() -- called from ISR stub on every key
// ============================================================

void Keyboard::irq1_handler(cinux::arch::InterruptFrame* /*frame*/) {
    uint8_t sc = io_inb(Ps2Port::DATA);

    // USB keyboard owns input: drain the PS/2 byte but do not feed the queue
    // (single producer for the SPSC event queue + ring buffer).
    if (usb_primary_) {
        return;
    }

    // Handle extended scan code prefix (0xE0) -- skip for now
    if (sc == ScanCode::EXTENDED) {
        return;
    }

    // Determine if this is a make (press) or break (release) code
    bool    pressed   = (sc & 0x80) == 0;
    uint8_t make_code = sc & 0x7F;

    // Track modifier keys regardless of press/release
    if (make_code == ScanCode::LSHIFT || make_code == ScanCode::RSHIFT) {
        shift_held_ = pressed;
    }
    if (make_code == ScanCode::LCTRL) {
        ctrl_held_ = pressed;
    }
    if (make_code == ScanCode::LALT) {
        alt_held_ = pressed;
    }

    // Translate to ASCII only on key press and if the make code is in range
    char ascii = 0;
    if (pressed && make_code < SCAN_TABLE_SIZE) {
        ascii = shift_held_ ? kScToUpper[make_code] : kScToLower[make_code];
    }

    dispatch_key(sc, ascii, pressed, shift_held_, ctrl_held_, alt_held_);
    // EOI is sent by the ISR_IRQ stub after this handler returns.
}

// ============================================================
// Keyboard::poll() -- dequeue an event from the ring buffer
// ============================================================

bool Keyboard::poll(KeyEvent& out) {
    cinux::proc::InterruptGuard guard;

    return buf_.pop(out);
}

// ============================================================
// Keyboard::enqueue() -- add event to the ring buffer
// ============================================================

void Keyboard::enqueue(const KeyEvent& ev) {
    // Drop the event if the buffer is full -- RingBuffer::push returns
    // false when full, matching the previous drop-newest semantics.
    static_cast<void>(buf_.push(ev));
}

// ============================================================
// Keyboard::set_usb_primary() / dispatch_key() / inject_usb_report()
// (USB boot-keyboard input path -- Batch 5B)
// ============================================================

namespace {
/// True if @p code appears in the first @p n entries of @p keys (edge detect).
bool key_in(const uint8_t* keys, uint8_t n, uint8_t code) {
    for (uint8_t i = 0; i < n; ++i) {
        if (keys[i] == code) {
            return true;
        }
    }
    return false;
}
}  // namespace

void Keyboard::set_usb_primary(bool primary) {
    usb_primary_ = primary;
}

void Keyboard::register_key_listener(KeyListener listener) {
    key_listener_ = listener;
}

void Keyboard::dispatch_key(uint8_t code, char ascii, bool pressed, bool shift, bool ctrl,
                            bool alt) {
    // Ctrl + letter -> control char (^C=0x03, ^D=0x04, ^Z=0x1A, ^\=0x1C ...).
    // The scan tables yield the base letter; Ctrl was tracked but not applied,
    // so Ctrl+C arrived as 'c' instead of 0x03 and never reached the TTY's
    // VINTR handling. Apply the standard PC ^X = X & 0x1F decoding for letters.
    if (ctrl && ascii != 0) {
        char lower = static_cast<char>(ascii | 0x20);  // fold to lowercase
        if (lower >= 'a' && lower <= 'z') {
            ascii = static_cast<char>(ascii & 0x1F);
        }
    }

    KeyEvent ev{};
    ev.scancode = code;
    ev.pressed  = pressed;
    ev.shift    = shift;
    ev.ctrl     = ctrl;
    ev.alt      = alt;
    ev.ascii    = ascii;
    enqueue(ev);

    // Dual dispatch: hand the event to a registered listener (e.g. the GUI
    // pushing it into its EventQueue).  No #ifdef here -- the keyboard driver
    // doesn't know about the GUI; whoever wants keys registers a listener
    // (CODING-TASTE §14).
    if (key_listener_ != nullptr) {
        key_listener_(ev);
    }

    // F10-M3 batch 2: feed the console TTY line discipline (stdin). Echo +
    // canonical editing happen here; sys_read fd==0 drains the cooked line.
    // The Backspace key arrives as ^H (VERASE, set in ConsoleTty::init) and
    // Enter already as '\n' from the scancode table.
    if (pressed && ascii != 0) {
        char c = (ascii == '\r') ? '\n' : ascii;
        console_tty().input(c);  // F10-M3: line discipline + signal + wake reader
    }
}

// Ctrl+letter -> POSIX control char (^A=0x01..^Z=0x1A); Ctrl+C becomes the
// 0x03 INTR the TTY line discipline maps to SIGINT.  HID only gives us the
// shifted/unshifted letter, so without this the INTR byte never reaches the
// tty and Ctrl+C can't interrupt a foreground process (e.g. busybox ping).
char apply_ctrl_modifier(char ascii, bool ctrl) {
    if (!ctrl) {
        return ascii;
    }
    if (ascii >= 'A' && ascii <= 'Z') {
        return static_cast<char>(ascii - 'A' + 1);
    }
    if (ascii >= 'a' && ascii <= 'z') {
        return static_cast<char>(ascii - 'a' + 1);
    }
    return ascii;
}

void Keyboard::inject_usb_report(uint8_t modifier, const uint8_t* keycodes, uint8_t n) {
    const bool shift = (modifier & (usb::HidKbdMod::kLShift | usb::HidKbdMod::kRShift)) != 0;
    const bool ctrl  = (modifier & (usb::HidKbdMod::kLCtrl | usb::HidKbdMod::kRCtrl)) != 0;
    const bool alt   = (modifier & (usb::HidKbdMod::kLAlt | usb::HidKbdMod::kRAlt)) != 0;
    shift_held_      = shift;
    ctrl_held_       = ctrl;
    alt_held_        = alt;

    // Press edges + autorepeat.  USB HID reports the same key every interval
    // (~10ms) while held; without software repeat the line discipline would
    // only see one backspace per keypress.  Arm a deadline on the first edge,
    // then re-emit at kUsbRepeatIntervalNs once it passes (Linux-style).
    const uint64_t now = g_hpet.available() ? g_hpet.monotonic_ns() : 0;
    for (uint8_t i = 0; i < n; ++i) {
        const uint8_t code = keycodes[i];
        if (code <= 1) {
            continue;  // 0 = none, 1 = rollover error
        }
        if (!key_in(usb_prev_keys_, 6, code)) {
            // new press edge -> dispatch + arm autorepeat
            char ascii = 0;
            if (code < usb::kHidKeymapSize) {
                ascii = shift ? usb::kHidShifted[code] : usb::kHidUnshifted[code];
            }
            ascii = apply_ctrl_modifier(ascii, ctrl);  // ^C=0x03 INTR, ^D=0x04 EOF, ^Z=0x1A SUSP
            dispatch_key(code, ascii, /*pressed=*/true, shift, ctrl, alt);
            usb_repeat_key_      = code;
            usb_repeat_deadline_ = now + kUsbRepeatDelayNs;
        } else if (now != 0 && code == usb_repeat_key_ && now >= usb_repeat_deadline_) {
            // held past the deadline -> emit a repeat
            char ascii = 0;
            if (code < usb::kHidKeymapSize) {
                ascii = shift ? usb::kHidShifted[code] : usb::kHidUnshifted[code];
            }
            ascii = apply_ctrl_modifier(ascii, ctrl);  // ^C=0x03 INTR, ^D=0x04 EOF, ^Z=0x1A SUSP
            dispatch_key(code, ascii, /*pressed=*/true, shift, ctrl, alt);
            usb_repeat_deadline_ = now + kUsbRepeatIntervalNs;
        }
    }

    // Release edges: in the previous report but not held now.
    for (uint8_t i = 0; i < 6; ++i) {
        const uint8_t code = usb_prev_keys_[i];
        if (code <= 1 || key_in(keycodes, n, code)) {
            continue;
        }
        dispatch_key(code, 0, /*pressed=*/false, shift, ctrl, alt);
        if (code == usb_repeat_key_) {
            usb_repeat_key_ = 0;  // releasing the repeating key stops autorepeat
        }
    }

    // Save this report for the next edge comparison.
    for (uint8_t i = 0; i < 6; ++i) {
        usb_prev_keys_[i] = (i < n) ? keycodes[i] : 0;
    }
}

}  // namespace cinux::drivers

// ============================================================
// C-linkage bridge: called from irq1_stub in interrupts.S
// ============================================================

extern "C" void keyboard_irq1_handler(cinux::arch::InterruptFrame* frame) {
    cinux::drivers::Keyboard::irq1_handler(frame);
}
