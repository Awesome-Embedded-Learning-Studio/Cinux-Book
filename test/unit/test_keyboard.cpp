#include <stdint.h>

#include "../framework/framework.hpp"
#include "cinux/container/ring_queue.hpp"
#include "kernel/driver/scancode.hpp"
#include "test_assert.hpp"

using cinux::base::container::RingQueue;
using cinux::driver::kScancodeLower;
using cinux::driver::kScancodeUpper;
using cinux::driver::TranslateScancode;
using cinux::driver::TranslatorState;

namespace {

TranslatorState fresh_state() {
    return TranslatorState{.shift_held = false, .extended = false};
}

}  // namespace

TEST("keyboard: tables pin the set-1 grammar") {
    ASSERT_EQ(kScancodeLower[0x02], '1');
    ASSERT_EQ(kScancodeLower[0x1E], 'a');
    ASSERT_EQ(kScancodeLower[0x39], ' ');
    ASSERT_EQ(kScancodeLower[0x35], '/');
    ASSERT_EQ(kScancodeUpper[0x02], '!');
    ASSERT_EQ(kScancodeUpper[0x1E], 'A');
    ASSERT_EQ(kScancodeUpper[0x35], '?');
    ASSERT_EQ(kScancodeUpper[0x29], '~');
}

TEST("keyboard: plain presses translate through the lower table") {
    TranslatorState state = fresh_state();
    ASSERT_EQ(TranslateScancode(state, 0x1E), 'a');
    ASSERT_EQ(TranslateScancode(state, 0x02), '1');
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST("keyboard: shift folds both seats and lifts the glyph") {
    TranslatorState state = fresh_state();
    ASSERT_EQ(TranslateScancode(state, 0x2A), 0);
    ASSERT_EQ(TranslateScancode(state, 0x1E), 'A');
    ASSERT_EQ(TranslateScancode(state, 0xB6), 0);
    ASSERT_EQ(TranslateScancode(state, 0x1E), 'a');
    ASSERT_EQ(TranslateScancode(state, 0xAA), 0);
}

TEST("keyboard: releases carry no event") {
    TranslatorState state = fresh_state();
    ASSERT_EQ(TranslateScancode(state, 0x9E), 0);
    ASSERT_EQ(TranslateScancode(state, 0x82), 0);
}

TEST("keyboard: the extended group drops whole") {
    TranslatorState state = fresh_state();
    ASSERT_EQ(TranslateScancode(state, 0xE0), 0);
    ASSERT_EQ(TranslateScancode(state, 0x1C), 0);
    ASSERT_EQ(TranslateScancode(state, 0x1E), 'a');
    ASSERT_EQ(TranslateScancode(state, 0xE0), 0);
    ASSERT_EQ(TranslateScancode(state, 0x9D), 0);
}

TEST("keyboard: queue is first in first out") {
    RingQueue<char, 8> queue;
    ASSERT_TRUE(queue.push('a'));
    ASSERT_TRUE(queue.push('s'));
    ASSERT_TRUE(queue.push('d'));
    char code = 0;
    ASSERT_TRUE(queue.pop(code));
    ASSERT_EQ(code, 'a');
    ASSERT_TRUE(queue.pop(code));
    ASSERT_EQ(code, 's');
    ASSERT_EQ(queue.count(), 1U);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST("keyboard: queue keeps one slot empty and drops on full") {
    RingQueue<char, 4> queue;
    ASSERT_TRUE(queue.push('1'));
    ASSERT_TRUE(queue.push('2'));
    ASSERT_TRUE(queue.push('3'));
    ASSERT_EQ(queue.count(), 3U);
    ASSERT_EQ(queue.push('4'), false);
    ASSERT_EQ(queue.count(), 3U);
    char code = 0;
    ASSERT_TRUE(queue.pop(code));
    ASSERT_EQ(code, '1');
    ASSERT_TRUE(queue.push('5'));
    ASSERT_EQ(queue.count(), 3U);
}

TEST("keyboard: empty queue refuses pops") {
    RingQueue<char, 8> queue;
    char               code = 0;
    ASSERT_EQ(queue.pop(code), false);
    ASSERT_EQ(queue.count(), 0U);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST("keyboard: queue wraps without losing order") {
    RingQueue<char, 4> queue;
    for (int round = 0; round < 3; ++round) {
        ASSERT_TRUE(queue.push('a'));
        ASSERT_TRUE(queue.push('b'));
        char code = 0;
        ASSERT_TRUE(queue.pop(code));
        ASSERT_EQ(code, 'a');
        ASSERT_TRUE(queue.pop(code));
        ASSERT_EQ(code, 'b');
    }
    ASSERT_EQ(queue.count(), 0U);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST("keyboard: queue survives many wrap cycles") {
    RingQueue<char, 3> queue;
    char               code = 0;
    for (int round = 0; round < 5; ++round) {
        ASSERT_TRUE(queue.push('x'));
        ASSERT_TRUE(queue.push('y'));
        ASSERT_TRUE(queue.pop(code));
        ASSERT_EQ(code, 'x');
        ASSERT_TRUE(queue.push('z'));
        ASSERT_TRUE(queue.pop(code));
        ASSERT_EQ(code, 'y');
        ASSERT_TRUE(queue.pop(code));
        ASSERT_EQ(code, 'z');
    }
    ASSERT_EQ(queue.count(), 0U);
}

int main() {
    return cinux::test::RunAll();
}
