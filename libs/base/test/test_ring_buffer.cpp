/**
 * @file test/unit/test_ring_buffer.cpp
 * @brief Tests for cinux::lib::RingBuffer<T, N>
 */

#include <catch2/catch_test_macros.hpp>
#include <cinux/ring_buffer.hpp>

using namespace cinux::lib;

TEST_CASE("RingBuffer: FIFO order", "[ring_buffer]") {
    RingBuffer<int, 8> rb;
    rb.push(1);
    rb.push(2);
    rb.push(3);

    int val = 0;
    REQUIRE(rb.pop(val));
    REQUIRE(val == 1);
    REQUIRE(rb.pop(val));
    REQUIRE(val == 2);
    REQUIRE(rb.pop(val));
    REQUIRE(val == 3);
}

TEST_CASE("RingBuffer: full returns false", "[ring_buffer]") {
    RingBuffer<int, 3> rb;
    REQUIRE(rb.push(1));
    REQUIRE(rb.push(2));
    REQUIRE(rb.push(3));
    REQUIRE_FALSE(rb.push(4));
    REQUIRE(rb.full());
}

TEST_CASE("RingBuffer: empty returns false", "[ring_buffer]") {
    RingBuffer<int, 4> rb;
    int                val;
    REQUIRE_FALSE(rb.pop(val));
    REQUIRE(rb.empty());
}

TEST_CASE("RingBuffer: wrap-around", "[ring_buffer]") {
    RingBuffer<int, 4> rb;
    // Fill and drain
    for (int i = 0; i < 4; ++i) {
        rb.push(i);
    }
    for (int i = 0; i < 4; ++i) {
        int v;
        rb.pop(v);
    }

    REQUIRE(rb.empty());
    // Now push again — wraps around
    for (int i = 10; i < 14; ++i) {
        rb.push(i);
    }

    int v;
    REQUIRE(rb.pop(v));
    REQUIRE(v == 10);
    REQUIRE(rb.pop(v));
    REQUIRE(v == 11);
}

TEST_CASE("RingBuffer: push_batch / pop_batch", "[ring_buffer]") {
    RingBuffer<int, 8> rb;
    int                src[] = {1, 2, 3, 4, 5};
    REQUIRE(rb.push_batch(src, 5) == 5);
    REQUIRE(rb.size() == 5);

    int dst[5] = {};
    REQUIRE(rb.pop_batch(dst, 5) == 5);
    REQUIRE(dst[0] == 1);
    REQUIRE(dst[4] == 5);
}

TEST_CASE("RingBuffer: clear", "[ring_buffer]") {
    RingBuffer<int, 4> rb;
    rb.push(1);
    rb.push(2);
    rb.clear();
    REQUIRE(rb.empty());
    REQUIRE(rb.size() == 0);
}

TEST_CASE("RingBuffer: boundary N=1", "[ring_buffer]") {
    RingBuffer<int, 1> rb;
    REQUIRE(rb.push(42));
    REQUIRE(rb.full());
    REQUIRE_FALSE(rb.push(99));

    int v;
    REQUIRE(rb.pop(v));
    REQUIRE(v == 42);
    REQUIRE(rb.empty());
}
