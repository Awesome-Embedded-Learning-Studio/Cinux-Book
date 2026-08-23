/**
 * @file host/test_evdev.cpp
 * @brief EvdevAccumulator unit test -- pure logic, no device (P1)
 *
 * Feeds synthetic input_event (type,code,value) triples through the accumulator
 * and checks the frames it produces at EV_SYN. This validates the evdev->pointer
 * translation offline, without /dev/input/event*. The real fbdev-host uses the
 * same accumulator against a live evdev fd.
 */

#include <cstdint>
#include <cstdio>

#include "evdev_device.hpp"

using namespace cinux::gui;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("FAIL: " __VA_ARGS__);                                 \
            std::printf("\n");                                                 \
            return 1;                                                          \
        }                                                                      \
    } while (0)

// evdev type/code constants (mirror <linux/input.h>; kept local so the test is
// self-contained and readable).
constexpr uint16_t kEvAbs = 0x03;
constexpr uint16_t kEvKey = 0x01;
constexpr uint16_t kAbsX  = 0x00;
constexpr uint16_t kAbsY  = 0x01;
constexpr uint16_t kBtnLeft = 0x110;

int main() {
    // 1. idle SYN (nothing fed) -> invalid
    {
        EvdevAccumulator a(100, 200);
        const EvdevFrame f = a.finish();
        CHECK(!f.valid, "empty SYN should be invalid");
    }

    // 2. ABS_X/Y move -> move frame at the new position
    {
        EvdevAccumulator a(100, 200);
        a.feed(kEvAbs, kAbsX, 150);
        a.feed(kEvAbs, kAbsY, 250);
        const EvdevFrame f = a.finish();
        CHECK(f.valid, "move SYN should be valid");
        CHECK(f.x == 150 && f.y == 250, "pos (%d,%d) expected (150,250)", f.x, f.y);
        CHECK(f.kind == kPointerKindMove, "kind %u expected move", f.kind);
        CHECK(f.buttons == 0u, "buttons %u expected 0", f.buttons);
    }

    // 3. BTN_LEFT press -> a down edge
    {
        EvdevAccumulator a(100, 200);
        a.feed(kEvKey, kBtnLeft, 1);
        const EvdevFrame f = a.finish();
        CHECK(f.valid, "press should be valid");
        CHECK(f.kind == kPointerKindDown, "kind %u expected down", f.kind);
        CHECK((f.buttons & 0x01u) != 0u, "left button should be set");
    }

    // 4. BTN_LEFT release (after a press) -> an up edge
    {
        EvdevAccumulator a(100, 200);
        a.feed(kEvKey, kBtnLeft, 1);
        a.finish();  // consume the press
        a.feed(kEvKey, kBtnLeft, 0);
        const EvdevFrame f = a.finish();
        CHECK(f.kind == kPointerKindUp, "kind %u expected up", f.kind);
        CHECK((f.buttons & 0x01u) == 0u, "left button should be clear");
    }

    // 5. move while button held -> move frame (button retained, no new edge)
    {
        EvdevAccumulator a(100, 200);
        a.feed(kEvKey, kBtnLeft, 1);
        a.finish();  // press consumed
        a.feed(kEvAbs, kAbsX, 120);
        const EvdevFrame f = a.finish();
        CHECK(f.kind == kPointerKindMove, "kind %u expected move (held, no edge)", f.kind);
        CHECK(f.x == 120, "x %d expected 120", f.x);
        CHECK((f.buttons & 0x01u) != 0u, "left still held");
    }

    // 6. a no-op value (same as current) -> idle SYN
    {
        EvdevAccumulator a(100, 200);
        a.feed(kEvAbs, kAbsX, 100);  // same as init x
        const EvdevFrame f = a.finish();
        CHECK(!f.valid, "same-value ABS_X should be idle");
    }

    // 7. right + middle button bits land in the right places
    {
        EvdevAccumulator a(100, 200);
        a.feed(kEvKey, /*BTN_RIGHT*/ 0x111, 1);
        a.feed(kEvKey, /*BTN_MIDDLE*/ 0x112, 1);
        const EvdevFrame f = a.finish();
        CHECK(f.buttons == 0x06u, "buttons 0x%02X expected 0x06 (right+middle)", f.buttons);
    }

    std::printf("evdev-test: OK (accumulator: idle/move/down/up/held-move/noop/multi-button)\n");
    return 0;
}
