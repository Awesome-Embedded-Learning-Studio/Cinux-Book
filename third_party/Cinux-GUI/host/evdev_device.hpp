/**
 * @file host/evdev_device.hpp
 * @brief Linux evdev backend -- read /dev/input/event* into pointer frames
 *
 * Host-side ONLY. An evdev device streams raw input_event records; a complete
 * pointer frame is the set of changes between two EV_SYN reports. EvdevAccumulator
 * is a pure (device-free) state machine that turns that stream into pointer
 * frames -- it is unit-tested offline with a fake stream. EvdevDevice wraps the
 * fd and feeds the accumulator.
 *
 * The pointer frame is then serialised by the host's poll_event into the
 * core's PointerPayload event ABI (the same one replay_host_main produces from
 * a script). Swapping the input source = swapping this one table callback.
 */
#pragma once

#include <cstdint>

#include "event_payload.hpp"  // kPointerKind*

namespace cinux::gui {

/**
 * @brief One accumulated pointer frame (produced at EV_SYN)
 *
 * @p valid is false until a SYN arrives with at least one field changed.
 */
struct EvdevFrame {
    int32_t  x       = 0;
    int32_t  y       = 0;
    uint8_t  buttons = 0;   // bit0=left, bit1=right, bit2=middle
    uint8_t  kind    = kPointerKindMove;  // down/up if a button edge happened
    bool     valid   = false;
};

/**
 * @brief Pure state machine: feed evdev input_event fields, harvest at SYN
 *
 * No fd, no syscall -- fully unit-testable. The host's poll_event drains the
 * evdev fd, feed()s each record, and on EV_SYN calls finish() to get one frame.
 */
class EvdevAccumulator {
public:
    explicit EvdevAccumulator(int32_t init_x = 0, int32_t init_y = 0)
        : x_(init_x), y_(init_y), start_x_(init_x), start_y_(init_y) {}

    /** Feed one input_event's (type, code, value). EV_SYN is handled by finish(). */
    void feed(uint16_t type, uint16_t code, int32_t value);

    /**
     * @brief Consume an EV_SYN: emit the accumulated frame and reset the deltas
     *
     * @return a frame with valid=true if anything changed since the last SYN,
     *         else valid=false (an idle SYN).
     */
    EvdevFrame finish();

private:
    int32_t x_ = 0, y_ = 0;          // absolute position (latest ABS_X/Y)
    uint8_t buttons_ = 0;            // latest button bitmask
    int32_t start_x_ = 0, start_y_ = 0;  // position at last SYN (move detection)
    uint8_t start_buttons_ = 0;      // buttons at last SYN (edge detection)
    bool    touched_ = false;        // any field changed since last SYN
    bool    button_edge_ = false;    // a button transition happened this frame
    uint8_t edge_kind_ = kPointerKindMove;  // down/up kind for the edge
};

/**
 * @brief Owning handle on a Linux evdev device (/dev/input/eventN)
 *
 * open() grabs the device (EVIOCGRAB) so the console doesn't also see the input.
 * read_frame() is non-blocking: it drains available input_event records through
 * the accumulator and returns the first complete frame (or valid==false).
 */
class EvdevDevice {
public:
    EvdevDevice() = default;
    ~EvdevDevice();

    EvdevDevice(const EvdevDevice&)            = delete;
    EvdevDevice& operator=(const EvdevDevice&) = delete;

    /** Open @p path (e.g. "/dev/input/event0"). Returns false on failure. */
    bool open(const char* path, int32_t init_x, int32_t init_y);

    void close();

    bool ready() const { return fd_ >= 0; }

    /**
     * @brief Drain pending input and return one accumulated frame
     *
     * Non-blocking. Returns frame.valid==false when no complete frame is ready.
     * Subsequent calls continue draining (the accumulator keeps partial state).
     */
    EvdevFrame read_frame();

private:
    int              fd_ = -1;
    EvdevAccumulator acc_;
};

}  // namespace cinux::gui
