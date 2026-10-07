#include <stdint.h>

#include <initializer_list>

#include "framework_kernel.hpp"
#include "kernel/driver/base/io.hpp"
#include "kernel/driver/keyboard.hpp"
#include "test_assert.hpp"
#include "test_case.hpp"  // NOLINT(misc-include-cleaner) consumed by the TEST() macro body

namespace {

constexpr uint16_t kDataPort    = 0x60;
constexpr uint16_t kCommandPort = 0x64;

constexpr uint8_t kStatusInputFull = 0x02;

constexpr uint8_t kCommandInjectByte = 0xD2;

constexpr unsigned long long kWaitSpinCap = 50000000ULL;

void inject_byte(uint8_t raw_byte) {
    cinux::driver::WaitBitsClear(kCommandPort, kStatusInputFull);
    cinux::driver::OutB({.port = kCommandPort, .value = kCommandInjectByte});
    cinux::driver::WaitBitsClear(kCommandPort, kStatusInputFull);
    cinux::driver::OutB({.port = kDataPort, .value = raw_byte});
}

bool await_event() {
    unsigned long long spins = 0;
    while (!cinux::driver::Keyboard::self().poll() && spins < kWaitSpinCap) {
        ++spins;
    }
    return spins < kWaitSpinCap;
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
