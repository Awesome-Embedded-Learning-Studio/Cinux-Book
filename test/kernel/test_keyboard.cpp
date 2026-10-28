#include <stdint.h>

#include <initializer_list>

#include "cinux/result.hpp"
#include "framework_kernel.hpp"
#include "kernel/driver/base/io.hpp"
#include "kernel/driver/input/event_stream.hpp"
#include "kernel/driver/input/input_config.hpp"
#include "kernel/driver/keyboard/keyboard.hpp"
#include "kernel/driver/mouse/ps2_mouse.hpp"
#include "kernel/syscall/sys_clock.hpp"
#include "kernel/syscall/syscall.hpp"
#include "test_assert.hpp"
#include "test_case.hpp"  // NOLINT(misc-include-cleaner) consumed by the TEST() macro body

namespace {

constexpr uint16_t kDataPort    = 0x60;
constexpr uint16_t kCommandPort = 0x64;

constexpr uint8_t kStatusInputFull = 0x02;

constexpr uint16_t kEvSynReport = 0;
constexpr uint16_t kEvRelWord   = 2;
constexpr uint16_t kRelXWord    = 0;
constexpr uint16_t kRelYWord    = 1;
constexpr uint16_t kBtnLeftWord = 0x110;

constexpr uint8_t kCommandInjectByte = 0xD2;

constexpr unsigned long long kWaitBudgetNanoseconds = 2000000000ULL;

bool within_budget(unsigned long long deadline) {
    return cinux::syscall::MonotonicNanoseconds() < deadline;
}

void inject_byte(uint8_t raw_byte) {
    cinux::driver::WaitBitsClear(kCommandPort, kStatusInputFull);
    cinux::driver::OutB({.port = kCommandPort, .value = kCommandInjectByte});
    cinux::driver::WaitBitsClear(kCommandPort, kStatusInputFull);
    cinux::driver::OutB({.port = kDataPort, .value = raw_byte});
}

bool await_event() {
    const unsigned long long kDeadline =
        cinux::syscall::MonotonicNanoseconds() + kWaitBudgetNanoseconds;
    while (!cinux::driver::Keyboard::self().poll()) {
        if (!within_budget(kDeadline)) {
            return false;
        }
    }
    return true;
}


constexpr uint8_t kCommandInjectSecond = 0xD3;

void inject_second(uint8_t raw_byte) {
    cinux::driver::WaitBitsClear(kCommandPort, kStatusInputFull);
    cinux::driver::OutB({.port = kCommandPort, .value = kCommandInjectSecond});
    cinux::driver::WaitBitsClear(kCommandPort, kStatusInputFull);
    cinux::driver::OutB({.port = kDataPort, .value = raw_byte});
}

bool await_stream(const cinux::driver::EventStream& stream) {
    const unsigned long long kDeadline =
        cinux::syscall::MonotonicNanoseconds() + kWaitBudgetNanoseconds;
    while (!stream.poll()) {
        if (!within_budget(kDeadline)) {
            return false;
        }
    }
    return true;
}

bool await_stream_owner(const cinux::driver::EventStream& stream, const void* owner) {
    const unsigned long long kDeadline =
        cinux::syscall::MonotonicNanoseconds() + kWaitBudgetNanoseconds;
    while (!stream.poll(owner)) {
        if (!within_budget(kDeadline)) {
            return false;
        }
    }
    return true;
}

using cinux::driver::InputEventWire;

void drain_stream(cinux::driver::EventStream& stream) {
    InputEventWire spent{};
    while (stream.read(&spent, sizeof(spent)).ok()) {
    }
}

bool take_record(cinux::driver::EventStream& stream, InputEventWire& record) {
    if (!await_stream(stream)) {
        return false;
    }
    const cinux::base::Result<unsigned long> kBytes = stream.read(&record, sizeof(record));
    return kBytes.ok();
}

}  // namespace

TEST("keyboard: an injected press lands in the queue") {
    inject_byte(0x1E);
    ASSERT_TRUE(await_event());
    ASSERT_EQ(cinux::driver::Keyboard::self().take(false), 'a');
}

TEST("keyboard: shift then press arrives lifted") {
    inject_byte(0x2A);
    inject_byte(0x1E);
    ASSERT_TRUE(await_event());
    ASSERT_EQ(cinux::driver::Keyboard::self().take(false), 'A');
    inject_byte(0xAA);
}

TEST("keyboard: a burst keeps its order") {
    inject_byte(0x1E);
    inject_byte(0x1F);
    inject_byte(0x20);
    for (char const kExpected : {'a', 's', 'd'}) {
        ASSERT_TRUE(await_event());
        ASSERT_EQ(cinux::driver::Keyboard::self().take(false), kExpected);
    }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) a body walks one story in order
TEST("keyboard: the injected press also reaches the event stream") {
    cinux::driver::EventStream& stream = cinux::driver::Keyboard::self().events();
    drain_stream(stream);
    inject_byte(0x1E);
    InputEventWire record{};
    ASSERT_TRUE(take_record(stream, record));
    ASSERT_EQ(record.type, cinux::driver::kEvKey);
    ASSERT_EQ(record.code, 30);
    ASSERT_EQ(record.value, 1);
    ASSERT_TRUE(take_record(stream, record));
    ASSERT_EQ(record.type, kEvSynReport);
    ASSERT_TRUE(await_event());
    ASSERT_EQ(cinux::driver::Keyboard::self().take(false), 'a');
}

TEST("keyboard: the extended arrow carries its own code") {
    cinux::driver::EventStream& stream = cinux::driver::Keyboard::self().events();
    drain_stream(stream);
    inject_byte(0xE0);
    inject_byte(0x50);
    InputEventWire record{};
    ASSERT_TRUE(take_record(stream, record));
    ASSERT_EQ(record.type, cinux::driver::kEvKey);
    ASSERT_EQ(record.code, 108);
    ASSERT_EQ(record.value, 1);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) a body walks one story in order
TEST("keyboard: grabbing quiets the glyph queue only") {
    cinux::driver::EventStream& stream  = cinux::driver::Keyboard::self().events();
    int                         session = 0;
    struct GrabCleanup {
        cinux::driver::EventStream& stream;
        const void*                 owner;
        ~GrabCleanup() { stream.release_owner(owner); }
    };
    const GrabCleanup kCleanup{.stream = stream, .owner = &session};
    drain_stream(stream);
    while (cinux::driver::Keyboard::self().poll()) {
        static_cast<void>(cinux::driver::Keyboard::self().take(false));
    }
    ASSERT_EQ(stream.answer(&session, cinux::driver::kEvdevGrab, 1), 0);
    ASSERT_EQ(stream.answer(&session, cinux::driver::kEvdevGrab, 1), -cinux::syscall::kEbusy);
    inject_byte(0x1E);
    InputEventWire record{};
    ASSERT_TRUE(await_stream_owner(stream, &session));
    ASSERT_TRUE(stream.read(&record, sizeof(record), &session).ok());
    ASSERT_EQ(record.code, 30);
    ASSERT_TRUE(!cinux::driver::Keyboard::self().poll());
    ASSERT_EQ(stream.answer(&session, cinux::driver::kEvdevGrab, 0), 0);
    inject_byte(0x1E);
    ASSERT_TRUE(await_event());
    ASSERT_EQ(cinux::driver::Keyboard::self().take(false), 'a');
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) a body walks one story in order
TEST("mouse: an injected packet becomes motion and button words") {
    ASSERT_TRUE(cinux::driver::Ps2Mouse::self().init());
    cinux::driver::EventStream& stream = cinux::driver::Ps2Mouse::self().events();
    inject_second(0x08);
    inject_second(0x03);
    inject_second(0x00);
    InputEventWire record{};
    ASSERT_TRUE(take_record(stream, record));
    ASSERT_EQ(record.type, kEvRelWord);
    ASSERT_EQ(record.code, kRelXWord);
    ASSERT_EQ(record.value, 3);
    ASSERT_TRUE(take_record(stream, record));
    ASSERT_EQ(record.type, kEvSynReport);

    inject_second(0x09);
    inject_second(0x00);
    inject_second(0x00);
    ASSERT_TRUE(take_record(stream, record));
    ASSERT_EQ(record.type, cinux::driver::kEvKey);
    ASSERT_EQ(record.code, kBtnLeftWord);
    ASSERT_EQ(record.value, 1);
    ASSERT_TRUE(take_record(stream, record));
    ASSERT_EQ(record.type, kEvSynReport);

    inject_second(0x39);
    inject_second(0xFE);
    inject_second(0xFA);
    ASSERT_TRUE(take_record(stream, record));
    ASSERT_EQ(record.type, kEvRelWord);
    ASSERT_EQ(record.code, kRelXWord);
    ASSERT_EQ(record.value, -2);
    ASSERT_TRUE(take_record(stream, record));
    ASSERT_EQ(record.type, kEvRelWord);
    ASSERT_EQ(record.code, kRelYWord);
    ASSERT_EQ(record.value, 6);
    ASSERT_TRUE(take_record(stream, record));
    ASSERT_EQ(record.type, kEvSynReport);

    inject_second(0x08);
    inject_second(0x00);
    inject_second(0x00);
    ASSERT_TRUE(take_record(stream, record));
    ASSERT_EQ(record.type, cinux::driver::kEvKey);
    ASSERT_EQ(record.code, kBtnLeftWord);
    ASSERT_EQ(record.value, 0);
    ASSERT_TRUE(take_record(stream, record));
    ASSERT_EQ(record.type, kEvSynReport);
}
