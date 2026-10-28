#include <stddef.h>
#include <stdint.h>

#include <utility>
#include <vector>

#include "../framework/framework.hpp"
#include "cinux/time.hpp"
#include "kernel/driver/ps2/controller.hpp"
#include "test_assert.hpp"

using cinux::driver::ps2::Ps2Ports;

namespace {

constexpr uint8_t kStatusOutputFull = 0x01;

/// A scripted controller: statuses answer in order and repeat the last
/// one; data bytes queue for reads; writes land in their records.
class Script {
public:
    explicit Script(std::vector<uint8_t> statuses, std::vector<uint8_t> replies)
        : statuses_(std::move(statuses)), replies_(std::move(replies)) {}

    Ps2Ports ports() {
        return {
            .context     = this,
            .status      = status_of,
            .data_in     = data_of,
            .command_out = command_of,
            .data_out    = write_of,
            .delay       = [](void*, cinux::base::time::Microseconds) {},
        };
    }

    [[nodiscard]] const std::vector<uint8_t>& commands() const { return commands_; }
    [[nodiscard]] const std::vector<uint8_t>& writes() const { return writes_; }
    [[nodiscard]] unsigned long               replies_taken() const { return taken_; }

private:
    uint8_t status() {
        const size_t kAt = status_at_ < statuses_.size() ? status_at_++ : statuses_.size() - 1U;
        return statuses_[kAt];
    }
    uint8_t data() {
        if (taken_ >= replies_.size()) {
            return 0;
        }
        return replies_[taken_++];
    }
    void command(uint8_t value) { commands_.push_back(value); }
    void write(uint8_t value) { writes_.push_back(value); }

    static uint8_t status_of(void* context) { return static_cast<Script*>(context)->status(); }
    static uint8_t data_of(void* context) { return static_cast<Script*>(context)->data(); }
    static void    command_of(void* context, uint8_t value) {
        static_cast<Script*>(context)->command(value);
    }
    static void write_of(void* context, uint8_t value) {
        static_cast<Script*>(context)->write(value);
    }

    std::vector<uint8_t> statuses_;
    std::vector<uint8_t> replies_;
    std::vector<uint8_t> commands_;
    std::vector<uint8_t> writes_;
    size_t               status_at_ = 0;
    unsigned long        taken_     = 0;
};

TEST("ps2 controller: one acknowledge closes the auxiliary transaction") {
    Script         script({0x00, kStatusOutputFull}, {0xFA});
    const Ps2Ports kPorts = script.ports();
    ASSERT_TRUE(cinux::driver::ps2::SendAux(kPorts, 0xF6));
    ASSERT_EQ(script.commands().size(), 1U);
    ASSERT_EQ(script.commands().at(0), cinux::driver::ps2::kCommandWriteAux);
    ASSERT_EQ(script.writes().size(), 1U);
    ASSERT_EQ(script.writes().at(0), 0xF6);
}

TEST("ps2 controller: a resend request is honoured once and then closes") {
    Script         script({0x00, kStatusOutputFull, 0x00, kStatusOutputFull}, {0xFE, 0xFA});
    const Ps2Ports kPorts = script.ports();
    ASSERT_TRUE(cinux::driver::ps2::SendAux(kPorts, 0xF4));
    ASSERT_EQ(script.replies_taken(), 2U);
    ASSERT_EQ(script.writes().size(), 2U);
}

TEST("ps2 controller: resends beyond the budget fail the transaction") {
    Script script({0x00, kStatusOutputFull, 0x00, kStatusOutputFull, 0x00, kStatusOutputFull},
                  {0xFE, 0xFE, 0xFE});
    const Ps2Ports kPorts = script.ports();
    ASSERT_TRUE(!cinux::driver::ps2::SendAux(kPorts, 0xF4));
    ASSERT_EQ(script.writes().size(), 3U);
}

TEST("ps2 controller: an error byte fails without a resend") {
    Script         script({0x00, kStatusOutputFull}, {0xFC});
    const Ps2Ports kPorts = script.ports();
    ASSERT_TRUE(!cinux::driver::ps2::SendAux(kPorts, 0xF6));
    ASSERT_EQ(script.writes().size(), 1U);
}

TEST("ps2 controller: a silent device fails the read within its budget") {
    Script         script({0x00, 0x00}, {});
    const Ps2Ports kPorts = script.ports();
    ASSERT_TRUE(!cinux::driver::ps2::SendAux(kPorts, 0xF6));
    ASSERT_EQ(script.replies_taken(), 0U);
}

TEST("ps2 controller: the drain drops every waiting byte") {
    Script         script({kStatusOutputFull, kStatusOutputFull, 0x00}, {0x12, 0x34});
    const Ps2Ports kPorts = script.ports();
    ASSERT_TRUE(cinux::driver::ps2::Drain(kPorts));
    ASSERT_EQ(script.replies_taken(), 2U);
}

}  // namespace

int main() {
    return cinux::test::RunAll();
}
TEST("review: auxiliary data waits until the controller consumes D4") {
    struct BusyController {
        unsigned int phase         = 0;
        bool         illegal_write = false;
    } controller;
    const Ps2Ports kPorts{
        .context = &controller,
        .status  = [](void* context) -> uint8_t {
            auto& state = *static_cast<BusyController*>(context);
            if (state.phase == 1) {
                state.phase = 2;
                return 0x02;
            }
            if (state.phase == 2) {
                state.phase = 3;
                return 0x00;
            }
            return state.phase == 4 ? 0x01 : 0x00;
        },
        .data_in     = [](void*) -> uint8_t { return 0xFA; },
        .command_out = [](void* context,
                          uint8_t) { static_cast<BusyController*>(context)->phase = 1; },
        .data_out =
            [](void* context, uint8_t) {
                auto& state         = *static_cast<BusyController*>(context);
                state.illegal_write = state.phase != 3;
                state.phase         = 4;
            },
        .delay = [](void*, cinux::base::time::Microseconds) {},
    };
    ASSERT_TRUE(cinux::driver::ps2::SendAux(kPorts, 0xF6));
    ASSERT_FALSE(controller.illegal_write);
}

TEST("ps2 controller: a busy input buffer refuses both kinds of write") {
    Script script({0x02}, {});
    ASSERT_FALSE(cinux::driver::ps2::SendCommand(script.ports(), 0xD4));
    ASSERT_FALSE(cinux::driver::ps2::WriteData(script.ports(), 0xF6));
    ASSERT_TRUE(script.commands().empty());
    ASSERT_TRUE(script.writes().empty());
}
