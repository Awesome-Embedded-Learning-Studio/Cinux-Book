#include "../framework/framework.hpp"
#include "cinux/result.hpp"
#include "kernel/interrupt/irq.hpp"
#include "kernel/interrupt/irq_config.hpp"
#include "test_assert.hpp"

namespace {

unsigned long g_message_fires = 0;

void count_one_message() {
    ++g_message_fires;
}

void count_two_messages() {
    g_message_fires += 2;
}

}  // namespace

TEST("irq: message seating refuses before a range is injected") {
    const cinux::base::Result<cinux::interrupt::MsiVector> kEarly =
        cinux::interrupt::Irq::self().seat_message(&count_one_message);
    ASSERT_FALSE(kEarly.ok());
    ASSERT_EQ(kEarly.error(), cinux::base::KernelError::kNotSupported);
}

TEST("irq: message vectors seat in the injected range and fire") {
    cinux::interrupt::Irq::self().init_message_range(0x40, cinux::interrupt::kMessageVectorCount);
    const cinux::base::Result<cinux::interrupt::MsiVector> kSeated =
        cinux::interrupt::Irq::self().seat_message(&count_one_message);
    ASSERT_TRUE(kSeated.ok());
    ASSERT_EQ(kSeated.value().value, 0x40U);

    cinux::interrupt::Irq::self().dispatch_message(kSeated.value());
    ASSERT_EQ(g_message_fires, 1UL);
}

TEST("irq: an unseated message vector drops quietly") {
    cinux::interrupt::Irq::self().dispatch_message(cinux::interrupt::MsiVector{.value = 0x41});
    ASSERT_EQ(g_message_fires, 1UL);
}

TEST("irq: the message pool answers full honestly") {
    for (unsigned long index = 1; index < cinux::interrupt::kMessageVectorCount; ++index) {
        const cinux::base::Result<cinux::interrupt::MsiVector> kFill =
            cinux::interrupt::Irq::self().seat_message(&count_two_messages);
        ASSERT_TRUE(kFill.ok());
        ASSERT_EQ(kFill.value().value, 0x40U + index);
    }
    const cinux::base::Result<cinux::interrupt::MsiVector> kFull =
        cinux::interrupt::Irq::self().seat_message(&count_two_messages);
    ASSERT_EQ(kFull.error(), cinux::base::KernelError::kOutOfMemory);

    cinux::interrupt::Irq::self().dispatch_message(cinux::interrupt::MsiVector{.value = 0x47});
    ASSERT_EQ(g_message_fires, 3UL);
}

int main() {
    return cinux::test::RunAll();
}
