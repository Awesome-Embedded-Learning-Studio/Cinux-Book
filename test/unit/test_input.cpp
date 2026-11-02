#include <stdint.h>

#include <atomic>
#include <thread>

#include "../framework/framework.hpp"
#include "cinux/result.hpp"
#include "kernel/driver/input/event_stream.hpp"
#include "kernel/driver/input/input_config.hpp"
#include "kernel/driver/keyboard/scancode.hpp"
#include "kernel/driver/mouse/mouse_packet.hpp"
#include "kernel/syscall/syscall.hpp"
#include "test_assert.hpp"

using cinux::driver::EventStream;
using cinux::driver::KeycodeEvent;
using cinux::driver::KeycodeState;
using cinux::driver::MousePacketDecoder;
using cinux::driver::MouseSample;
using cinux::driver::TranslateScancodeKeycode;

// NOLINTNEXTLINE(readability-function-cognitive-complexity) a body walks one story in order
TEST("input: a published report reads back as whole wire records") {
    EventStream stream;
    ASSERT_TRUE(!stream.poll());
    const cinux::base::Result<unsigned long> kEmpty =
        stream.read(nullptr, cinux::driver::kInputEventBytes);
    ASSERT_TRUE(kEmpty.error() == cinux::base::KernelError::kWouldBlock);

    stream.publish(cinux::driver::kEvKey, 30, 1);
    stream.publish_report();
    ASSERT_TRUE(stream.poll());

    cinux::driver::InputEventWire            records[2]{};
    const cinux::base::Result<unsigned long> kBytes = stream.read(records, sizeof(records));
    ASSERT_TRUE(kBytes.ok());
    ASSERT_EQ(kBytes.value(), 2 * cinux::driver::kInputEventBytes);
    ASSERT_EQ(records[0].type, cinux::driver::kEvKey);
    ASSERT_EQ(records[0].code, 30);
    ASSERT_EQ(records[0].value, 1);
    ASSERT_EQ(records[1].type, 0);
    ASSERT_EQ(records[1].code, 0);
    ASSERT_TRUE(!stream.poll());
}

TEST("input: a short offer reads nothing and refuses") {
    EventStream stream;
    stream.publish(2, 0, 5);
    const cinux::base::Result<unsigned long> kShort = stream.read(nullptr, 23);
    ASSERT_TRUE(kShort.error() == cinux::base::KernelError::kInvalidArgument);
    ASSERT_TRUE(stream.poll());
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) a body walks one story in order
TEST("input: grabs belong to independent opens and final close releases them") {
    EventStream stream;
    int         first  = 0;
    int         second = 0;
    bool        heard  = false;
    stream.set_grab_hook(
        [](void* context, bool grabbed) { *static_cast<bool*>(context) = grabbed; }, &heard);
    ASSERT_EQ(stream.answer(&first, cinux::driver::kEvdevGrab, 1), 0);
    ASSERT_TRUE(heard);
    ASSERT_EQ(stream.answer(&first, cinux::driver::kEvdevGrab, 1), -cinux::syscall::kEbusy);
    ASSERT_EQ(stream.answer(&second, cinux::driver::kEvdevGrab, 1), -cinux::syscall::kEbusy);
    ASSERT_EQ(stream.answer(&second, cinux::driver::kEvdevGrab, 0), -cinux::syscall::kEinval);
    stream.publish(1, 30, 1);
    ASSERT_FALSE(stream.poll(&second));
    ASSERT_TRUE(stream.poll(&first));
    stream.release_owner(&second);
    ASSERT_TRUE(heard);
    stream.release_owner(&first);
    ASSERT_FALSE(heard);
    ASSERT_TRUE(stream.poll(&second));
    ASSERT_EQ(stream.answer(&second, cinux::driver::kEvdevGrab, 1), 0);
    ASSERT_EQ(stream.answer(&second, cinux::driver::kEvdevGrab, 0), 0);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) a body walks one story in order
TEST("input: overflow releases delivered keys and reports the dropped boundary") {
    EventStream                   stream;
    cinux::driver::InputEventWire record{};
    stream.publish(1, 0x110, 1);
    ASSERT_TRUE(stream.read(&record, sizeof(record)).ok());
    for (unsigned long index = 0; index < cinux::driver::kInputEventQueueCapacity; ++index) {
        stream.publish(2, 0, 1);
    }
    stream.publish(1, 0x110, 0);
    stream.publish_report();
    ASSERT_TRUE(stream.read(&record, sizeof(record)).ok());
    ASSERT_EQ(record.type, 0);
    ASSERT_EQ(record.code, 3);
    ASSERT_TRUE(stream.read(&record, sizeof(record)).ok());
    ASSERT_EQ(record.type, 1);
    ASSERT_EQ(record.code, 0x110);
    ASSERT_EQ(record.value, 0);
    ASSERT_TRUE(stream.read(&record, sizeof(record)).ok());
    ASSERT_EQ(record.type, 0);
    ASSERT_EQ(record.code, 0);
}

TEST("input: concurrent producer and consumer preserve a recoverable released state") {
    EventStream        stream;
    std::atomic<bool>  finished{false};
    const std::jthread kProducer([&] {
        for (unsigned long turn = 0; turn < 10000; ++turn) {
            stream.publish(1, 30, 1);
            stream.publish_report();
            stream.publish(1, 30, 0);
            stream.publish_report();
        }
        finished.store(true);
    });
    bool               pressed = false;
    while (!finished.load() || stream.poll()) {
        cinux::driver::InputEventWire record{};
        if (stream.read(&record, sizeof(record)).ok() && record.type == 1 && record.code == 30) {
            pressed = record.value != 0;
        }
    }
    ASSERT_FALSE(pressed);
}

TEST("scancode: the main block's make codes are the event codes") {
    KeycodeState state;
    KeycodeEvent event{};
    ASSERT_TRUE(TranslateScancodeKeycode(state, 0x1E, event));
    ASSERT_EQ(event.code, 30);
    ASSERT_TRUE(event.pressed);
    ASSERT_TRUE(TranslateScancodeKeycode(state, 0x1EU | 0x80U, event));
    ASSERT_EQ(event.code, 30);
    ASSERT_TRUE(!event.pressed);
}

TEST("scancode: the leader passes through and the arrow answers") {
    KeycodeState state;
    KeycodeEvent event{};
    ASSERT_TRUE(!TranslateScancodeKeycode(state, 0xE0, event));
    ASSERT_TRUE(TranslateScancodeKeycode(state, 0x50, event));
    ASSERT_EQ(event.code, 108);
    ASSERT_TRUE(!TranslateScancodeKeycode(state, 0xE0, event));
    ASSERT_TRUE(TranslateScancodeKeycode(state, 0x4B, event));
    ASSERT_EQ(event.code, 105);
}

TEST("mouse: a plain packet decodes with its motion and buttons") {
    MousePacketDecoder decoder;
    MouseSample        sample{};
    ASSERT_TRUE(!decoder.feed(0x09, sample));
    ASSERT_TRUE(!decoder.feed(0x02, sample));
    ASSERT_TRUE(decoder.feed(0x03, sample));
    ASSERT_EQ(sample.dx, 2);
    ASSERT_EQ(sample.dy, -3);
    ASSERT_TRUE(sample.left);
    ASSERT_TRUE(!sample.right);
    ASSERT_TRUE(!sample.middle);
}

TEST("mouse: a hunting decoder resynchronizes on the next head") {
    MousePacketDecoder decoder;
    MouseSample        sample{};
    ASSERT_TRUE(!decoder.feed(0x03, sample));
    ASSERT_TRUE(!decoder.feed(0x09, sample));
    ASSERT_TRUE(!decoder.feed(0x01, sample));
    ASSERT_TRUE(decoder.feed(0x00, sample));
    ASSERT_EQ(sample.dx, 1);
    ASSERT_EQ(sample.dy, 0);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) a body walks one story in order
TEST("mouse: negative motion extends the sign bits") {
    MousePacketDecoder decoder;
    MouseSample        sample{};
    ASSERT_TRUE(!decoder.feed(0x18, sample));
    ASSERT_TRUE(!decoder.feed(0xFE, sample));
    ASSERT_TRUE(decoder.feed(0x00, sample));
    ASSERT_EQ(sample.dx, -2);
    ASSERT_EQ(sample.dy, 0);

    ASSERT_TRUE(!decoder.feed(0x28, sample));
    ASSERT_TRUE(!decoder.feed(0x00, sample));
    ASSERT_TRUE(decoder.feed(0xFE, sample));
    ASSERT_EQ(sample.dx, 0);
    ASSERT_EQ(sample.dy, 2);
}

TEST("input: absolute axes expose their range and latest coordinates") {
    EventStream stream;
    stream.configure_absolute(32767);
    stream.publish(3, 0, 12345);
    stream.publish(3, 1, 32767);
    cinux::driver::InputAbsInfo info{};
    ASSERT_EQ(stream.answer(nullptr, cinux::driver::kEvdevGetAbsX,
                            reinterpret_cast<unsigned long long>(&info)),
              0);
    ASSERT_EQ(info.minimum, 0);
    ASSERT_EQ(info.maximum, 32767);
    ASSERT_EQ(info.value, 12345);
    ASSERT_EQ(stream.answer(nullptr, cinux::driver::kEvdevGetAbsY,
                            reinterpret_cast<unsigned long long>(&info)),
              0);
    ASSERT_EQ(info.value, 32767);
    EventStream relative;
    ASSERT_EQ(relative.answer(nullptr, cinux::driver::kEvdevGetAbsX,
                              reinterpret_cast<unsigned long long>(&info)),
              -cinux::syscall::kEnotty);
}

TEST("input: a dropped burst announces the loss before the axis replay") {
    EventStream stream;
    stream.configure_absolute(32767);
    for (unsigned long index = 0; index < cinux::driver::kInputEventQueueCapacity; ++index) {
        stream.publish(3, 0, 1);
    }
    stream.publish(3, 0, 30000);
    stream.publish(3, 1, 25000);
    stream.publish_report();
    cinux::driver::InputEventWire records[2]{};
    ASSERT_TRUE(stream.read(records, sizeof(records)).ok());
    ASSERT_EQ(records[0].type, 0);
    ASSERT_EQ(records[0].code, 3);
    ASSERT_EQ(records[1].type, cinux::driver::kEvAbs);
    ASSERT_EQ(records[1].code, 0);
    ASSERT_EQ(records[1].value, 30000);
}

TEST("input: absolute replay closes with the second axis and one report word") {
    EventStream stream;
    stream.configure_absolute(32767);
    for (unsigned long index = 0; index < cinux::driver::kInputEventQueueCapacity; ++index) {
        stream.publish(3, 0, 1);
    }
    stream.publish(3, 0, 30000);
    stream.publish(3, 1, 25000);
    stream.publish_report();
    cinux::driver::InputEventWire records[4]{};
    ASSERT_TRUE(stream.read(records, sizeof(records)).ok());
    ASSERT_EQ(records[2].type, cinux::driver::kEvAbs);
    ASSERT_EQ(records[2].code, 1);
    ASSERT_EQ(records[2].value, 25000);
    ASSERT_EQ(records[3].type, 0);
    ASSERT_EQ(records[3].code, 0);
}

int main() {
    return cinux::test::RunAll();
}
