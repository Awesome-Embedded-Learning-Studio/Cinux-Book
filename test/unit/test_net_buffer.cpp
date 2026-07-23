/**
 * @file test/unit/test_net_buffer.cpp
 * @brief Host unit tests for the Packet/BufferSink buffer abstraction in
 *        isolation -- default state, recycle dispatch, and the SCOPE_EXIT
 *        recycle pattern (the mechanism NetStack::poll relies on) firing on
 *        both normal exit and early drop.
 *
 * Header-only types (kernel/net/buffer.hpp + cinux::lib::ScopeGuard).
 *
 * Compile condition: -DCINUX_HOST_TEST
 */

#define TEST_FRAMEWORK_IMPL

#include <cinux/scope_guard.hpp>
#include <cstdint>

#include "kernel/net/buffer.hpp"
#include "test_framework.h"

using cinux::net::BufferSink;
using cinux::net::Packet;

namespace {

struct CountingSink : BufferSink {
    int            calls = 0;
    const uint8_t* last  = nullptr;
    void           recycle(const uint8_t* data) override {
        ++calls;
        last = data;
    }
};

}  // namespace

TEST("buffer: default Packet is empty with no sink") {
    Packet p;
    ASSERT_TRUE(p.data == nullptr);
    ASSERT_EQ(p.len, 0u);
    ASSERT_TRUE(p.sink == nullptr);
}

TEST("buffer: BufferSink::recycle records each call") {
    CountingSink  s;
    const uint8_t b[4] = {1, 2, 3, 4};
    s.recycle(b);
    s.recycle(b + 2);
    ASSERT_EQ(s.calls, 2);
    ASSERT_TRUE(s.last == b + 2);
}

TEST("buffer: scope guard recycles a borrowed packet on normal exit") {
    CountingSink s;
    uint8_t      storage[8] = {};
    Packet       pkt{storage, 8, &s};
    {
        SCOPE_EXIT(if (pkt.sink != nullptr) { pkt.sink->recycle(pkt.data); });
        // ... handler would run here ...
    }  // guard fires
    ASSERT_EQ(s.calls, 1);
}

TEST("buffer: scope guard recycles on early drop (return)") {
    CountingSink s;
    uint8_t      storage[8] = {};
    Packet       pkt{storage, 8, &s};

    auto drop_like = [&]() {
        SCOPE_EXIT(if (pkt.sink != nullptr) { pkt.sink->recycle(pkt.data); });
        return;  // early exit -- must still recycle
    };
    drop_like();

    ASSERT_EQ(s.calls, 1);  // not leaked on the drop path
}

TEST("buffer: copy device (sink==nullptr) guard is a no-op") {
    uint8_t storage[8] = {};
    Packet  pkt{storage, 8, nullptr};  // copy device -- no sink
    {
        SCOPE_EXIT(if (pkt.sink != nullptr) { pkt.sink->recycle(pkt.data); });
    }
    // No sink -> no deref, no crash. Nothing to assert except "we got here".
    ASSERT_TRUE(pkt.sink == nullptr);
}

int main() {
    RUN_ALL_TESTS();
    return _tests_failed > 0 ? 1 : 0;
}
