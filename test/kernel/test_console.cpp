#include <stdint.h>

#include "cinux/ptr.hpp"
#include "framework_kernel.hpp"
#include "kernel/console/screen.hpp"
#include "kernel/driver/framebuffer/framebuffer.hpp"
#include "kernel/mm/layout.hpp"
#include "test_assert.hpp"
#include "test_case.hpp"  // NOLINT(misc-include-cleaner) consumed by the TEST() macro body

namespace {

unsigned long screen_sum() {
    const cinux::driver::Framebuffer& screen = cinux::driver::Framebuffer::self();
    const auto* const                 kBase  = cinux::base::PtrAt<const volatile uint32_t>(
        cinux::mm::IoremapVirt(static_cast<unsigned long>(screen.layout().physical)));
    const unsigned long kStride = screen.layout().pitch / 4U;
    unsigned long       total   = 0;
    for (uint32_t row = 0; row < screen.height(); ++row) {
        for (uint32_t column = 0; column < screen.width(); ++column) {
            total += kBase[(row * kStride) + column];
        }
    }
    return total;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) a body walks one story in order
TEST("console frame: every holder keeps the text screen dark until the last leaves") {
    cinux::console::TextConsole& console = cinux::console::TextConsole::self();
    console.yield_frame();
    console.yield_frame();
    const unsigned long kHeld = screen_sum();
    console.put_char('Z');
    ASSERT_EQ(screen_sum(), kHeld);
    console.reclaim_frame();
    console.put_char('Y');
    ASSERT_EQ(screen_sum(), kHeld);
    console.reclaim_frame();
    console.put_char('X');
    ASSERT_TRUE(screen_sum() != kHeld);
}
}  // namespace
