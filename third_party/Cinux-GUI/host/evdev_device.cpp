/**
 * @file host/evdev_device.cpp
 * @brief Linux evdev backend implementation
 *
 * Host-side ONLY. Uses <linux/input.h> read/input_event + EVIOCGRAB. The
 * accumulator (feed/finish) is pure and unit-tested separately.
 */

#include "evdev_device.hpp"

#include <cstdint>
#include <cstring>

#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>

#ifndef EV_SYN
#define EV_SYN 0x00
#endif
#ifndef SYN_REPORT
#define SYN_REPORT 0x00
#endif
#ifndef SYN_DROPPED
#define SYN_DROPPED 0x03
#endif

namespace cinux::gui {

namespace {

// Absolute axes + button codes we recognise. Other codes are ignored.
constexpr uint16_t kEvAbs = 0x03;   // EV_ABS
constexpr uint16_t kEvKey = 0x01;   // EV_KEY
constexpr uint16_t kAbsX  = 0x00;   // ABS_X
constexpr uint16_t kAbsY  = 0x01;   // ABS_Y
constexpr uint16_t kBtnLeft   = 0x110;  // BTN_LEFT
constexpr uint16_t kBtnRight  = 0x111;  // BTN_RIGHT
constexpr uint16_t kBtnMiddle = 0x112;  // BTN_MIDDLE

}  // namespace

void EvdevAccumulator::feed(uint16_t type, uint16_t code, int32_t value) {
    if (type == kEvAbs) {
        if (code == kAbsX) {
            if (value != x_) {
                touched_ = true;
            }
            x_ = value;
        } else if (code == kAbsY) {
            if (value != y_) {
                touched_ = true;
            }
            y_ = value;
        }
    } else if (type == kEvKey) {
        uint8_t bit = 0;
        if (code == kBtnLeft) {
            bit = 0x01u;
        } else if (code == kBtnRight) {
            bit = 0x02u;
        } else if (code == kBtnMiddle) {
            bit = 0x04u;
        } else {
            return;
        }
        const uint8_t mask = bit;
        const uint8_t prev = buttons_;
        if (value != 0) {
            buttons_ |= mask;
        } else {
            buttons_ &= static_cast<uint8_t>(~mask);
        }
        if (buttons_ != prev) {
            touched_      = true;
            button_edge_  = true;
            // A press wins over a release if both happen in one frame (rare).
            edge_kind_ = (value != 0) ? kPointerKindDown : kPointerKindUp;
        }
    }
    // other types (EV_SYN handled by finish, EV_REL/MSC/etc. ignored)
}

EvdevFrame EvdevAccumulator::finish() {
    EvdevFrame f;
    f.x       = x_;
    f.y       = y_;
    f.buttons = buttons_;
    f.kind    = button_edge_ ? edge_kind_ : kPointerKindMove;
    f.valid   = touched_;
    // reset per-frame state for the next SYN interval
    start_x_       = x_;
    start_y_       = y_;
    start_buttons_ = buttons_;
    touched_       = false;
    button_edge_   = false;
    edge_kind_     = kPointerKindMove;
    return f;
}

EvdevDevice::~EvdevDevice() {
    close();
}

bool EvdevDevice::open(const char* path, int32_t init_x, int32_t init_y) {
    fd_ = ::open(path, O_RDONLY | O_NONBLOCK);
    if (fd_ < 0) {
        return false;
    }
    // Grab the device so the console TTY doesn't also receive the input.
    // Best-effort: ignore EBUSY (already grabbed) -- we still read.
    ::ioctl(fd_, EVIOCGRAB, 1);
    acc_ = EvdevAccumulator(init_x, init_y);
    return true;
}

void EvdevDevice::close() {
    if (fd_ >= 0) {
        ::ioctl(fd_, EVIOCGRAB, 0);
        ::close(fd_);
        fd_ = -1;
    }
}

EvdevFrame EvdevDevice::read_frame() {
    if (fd_ < 0) {
        return {};
    }
    // Drain whatever is available. Stop on a completed frame or no more data.
    for (;;) {
        input_event ev{};
        ssize_t n = ::read(fd_, &ev, sizeof(ev));
        if (n != static_cast<ssize_t>(sizeof(ev))) {
            break;  // EAGAIN (nothing more) or partial -- wait for next call
        }
        if (ev.type == EV_SYN) {
            if (ev.code == SYN_DROPPED) {
                // kernel dropped events; the accumulator's last known state
                // is our best guess -- treat the next SYN as a fresh start.
                acc_ = EvdevAccumulator();  // revisit: could query EVIOCGABS here
                continue;
            }
            return acc_.finish();
        }
        acc_.feed(static_cast<uint16_t>(ev.type), static_cast<uint16_t>(ev.code),
                  ev.value);
    }
    return {};  // no complete frame this call
}

}  // namespace cinux::gui
