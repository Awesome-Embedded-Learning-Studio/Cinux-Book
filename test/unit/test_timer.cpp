#include "../framework/framework.hpp"
#include "kernel/time/timer_config.hpp"
#include "kernel/time/timer_table.hpp"
#include "test_assert.hpp"

namespace {

struct Tally {
    unsigned long fires = 0;

    static void ring(void* context) { ++static_cast<Tally*>(context)->fires; }
};

}  // namespace

TEST("timer: a seat fires once at or past its deadline") {
    auto&      table = cinux::time::TimerTable::self();
    Tally      tally;
    const auto kSeat = table.arm(&tally, &Tally::ring, 1000);
    ASSERT_TRUE(kSeat.ok());

    table.advance(999);
    ASSERT_TRUE(tally.fires == 0);
    table.advance(1000);
    ASSERT_TRUE(tally.fires == 1);
    table.advance(5000);
    ASSERT_TRUE(tally.fires == 1);
}

TEST("timer: a cancelled seat stays silent") {
    auto&      table = cinux::time::TimerTable::self();
    Tally      tally;
    const auto kSeat = table.arm(&tally, &Tally::ring, 1000);
    ASSERT_TRUE(kSeat.ok());
    ASSERT_TRUE(table.cancel(kSeat.value()));

    table.advance(5000);
    ASSERT_TRUE(tally.fires == 0);
    ASSERT_FALSE(table.cancel(kSeat.value()));
}

TEST("timer: a fired seat's ticket is stale for cancelling") {
    auto&      table = cinux::time::TimerTable::self();
    Tally      tally;
    const auto kFirst = table.arm(&tally, &Tally::ring, 100);
    ASSERT_TRUE(kFirst.ok());
    table.advance(100);
    ASSERT_TRUE(tally.fires == 1);

    const auto kSecond = table.arm(&tally, &Tally::ring, 200);
    ASSERT_TRUE(kSecond.ok());
    ASSERT_TRUE(kSecond.value().slot == kFirst.value().slot);
    ASSERT_TRUE(kSecond.value().stamp == kFirst.value().stamp + 2);
    ASSERT_FALSE(table.cancel(kFirst.value()));
    table.advance(100);
    ASSERT_TRUE(tally.fires == 1);
    table.advance(200);
    ASSERT_TRUE(tally.fires == 2);
}

TEST("timer: a callback may re-arm its own seat mid-advance") {
    auto&      table = cinux::time::TimerTable::self();
    Tally      tally;
    const auto kSeat = table.arm(&tally, &Tally::ring, 100);
    ASSERT_TRUE(kSeat.ok());

    table.advance(100);
    ASSERT_TRUE(tally.fires == 1);
    const auto kAgain = table.arm(&tally, &Tally::ring, 300);
    ASSERT_TRUE(kAgain.ok());
    table.advance(200);
    ASSERT_TRUE(tally.fires == 1);
    table.advance(300);
    ASSERT_TRUE(tally.fires == 2);
}

TEST("timer: a full table refuses by name") {
    auto& table = cinux::time::TimerTable::self();
    Tally tally;
    for (unsigned long attempt = 0; attempt < cinux::time::kTimerSeats; ++attempt) {
        const auto kSeat = table.arm(&tally, &Tally::ring, 100 + attempt);
        ASSERT_TRUE(kSeat.ok());
    }
    const auto kExtra = table.arm(&tally, &Tally::ring, 999);
    ASSERT_FALSE(kExtra.ok());
    table.advance(1000);
    ASSERT_TRUE(tally.fires == cinux::time::kTimerSeats);
}

TEST("timer: seats fire in seat order in one pass") {
    auto&      table = cinux::time::TimerTable::self();
    Tally      tally;
    const auto kLate  = table.arm(&tally, &Tally::ring, 500);
    const auto kEarly = table.arm(&tally, &Tally::ring, 100);
    ASSERT_TRUE(kLate.ok());
    ASSERT_TRUE(kEarly.ok());

    table.advance(500);
    ASSERT_TRUE(tally.fires == 2);
    ASSERT_FALSE(table.cancel(kEarly.value()));
    ASSERT_FALSE(table.cancel(kLate.value()));
}

int main() {
    return cinux::test::RunAll();
}
