#include "../framework/framework.hpp"
#include "kernel/driver/nvme/nvme_layout.hpp"
#include "test_assert.hpp"

TEST("nvme: completion identity, queue and phase all belong to the submitted command") {
    cinux::driver::NvmeCompletion entry{.queue = 1, .identity = 7, .status = 1};
    ASSERT_TRUE(cinux::driver::NvmeCompleted(entry, 7, 1, true));
    ASSERT_FALSE(cinux::driver::NvmeCompleted(entry, 8, 1, true));
    ASSERT_FALSE(cinux::driver::NvmeCompleted(entry, 7, 0, true));
    ASSERT_FALSE(cinux::driver::NvmeCompleted(entry, 7, 1, false));
    entry.status = 0;
    ASSERT_TRUE(cinux::driver::NvmeCompleted(entry, 7, 1, false));
}

TEST("nvme: the historical 4080 error remains a failure in either ring phase") {
    cinux::driver::NvmeCompletion entry{.queue = 1, .identity = 7, .status = 0x8101};
    ASSERT_FALSE(cinux::driver::NvmeCompleted(entry, 7, 1, true));
    entry.status = 0x8100;
    ASSERT_FALSE(cinux::driver::NvmeCompleted(entry, 7, 1, false));
    entry.status = 0x4081;
    ASSERT_FALSE(cinux::driver::NvmeCompleted(entry, 7, 1, true));
}

int main() {
    return cinux::test::RunAll();
}
