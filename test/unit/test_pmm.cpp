#include <stdint.h>

#include "../framework/framework.hpp"
#include "cinux/addr.hpp"
#include "cinux/literal_types.hpp"
#include "kernel/arch/x86_64/page.hpp"
#include "kernel/boot/boot_info.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/pmm_config.hpp"

using cinux::base::PhysAddr;
using cinux::mm::Pmm;

using cinux::base::operator""_GiB;
using cinux::base::operator""_KiB;
using cinux::base::operator""_MiB;

namespace {

cinux::boot::E820Entry make_entry(unsigned long base, unsigned long length, uint32_t type) {
    return {.base = base, .length = length, .type = type};
}

}  // namespace

TEST("pmm: constants are pinned") {
    ASSERT_TRUE(cinux::arch::page::kSize == 4_KiB);
    ASSERT_TRUE(cinux::mm::kLowMemoryTop == 1_MiB);
    ASSERT_TRUE(cinux::mm::kPmmMaxPhys == 16_GiB);
    ASSERT_TRUE(cinux::mm::kMaxOrder == 9);
    ASSERT_TRUE(cinux::boot::kE820Usable == 1);
}

TEST("pmm: low megabyte stays out of the ledger") {
    cinux::boot::E820Entry const kMap[] = {
        make_entry(0, 128_KiB, 1), make_entry(128_KiB, 896_KiB, 2), make_entry(1_MiB, 4_MiB, 1)};
    Pmm pmm{};
    ASSERT_TRUE(pmm.init(kMap, 3, PhysAddr{2_MiB}, 8_KiB));
    ASSERT_TRUE(pmm.managed_page_count() == 1024);
    ASSERT_TRUE(pmm.free_page_count() == 1022);
    ASSERT_TRUE(pmm.allocate_page() == PhysAddr{1_MiB});
}

TEST("pmm: order runs respect kernel holes") {
    cinux::boot::E820Entry const kMap[] = {make_entry(1_MiB, 4_MiB, 1)};
    Pmm                          pmm{};
    ASSERT_TRUE(pmm.init(kMap, 1, PhysAddr{2_MiB}, 8_KiB));
    unsigned long const kAfterKernel = 2_MiB + 8_KiB;
    ASSERT_TRUE(pmm.allocate_pages(9) == PhysAddr{kAfterKernel});
    ASSERT_TRUE(pmm.free_page_count() == 1022 - 512);
    ASSERT_TRUE(pmm.allocate_pages(9) == PhysAddr{});
    pmm.free_pages(PhysAddr{kAfterKernel}, 9);
    ASSERT_TRUE(pmm.free_page_count() == 1022);
}

TEST("pmm: order bounds are refused") {
    cinux::boot::E820Entry const kMap[] = {make_entry(1_MiB, 4_MiB, 1)};
    Pmm                          pmm{};
    ASSERT_TRUE(pmm.init(kMap, 1, PhysAddr{2_MiB}, 8_KiB));
    ASSERT_TRUE(pmm.allocate_pages(-1) == PhysAddr{});
    ASSERT_TRUE(pmm.allocate_pages(10) == PhysAddr{});
    ASSERT_TRUE(pmm.free_page_count() == 1022);
}

TEST("pmm: every free page is reachable exactly once") {
    cinux::boot::E820Entry const kMap[] = {make_entry(1_MiB, 4_MiB, 1)};
    Pmm                          pmm{};
    ASSERT_TRUE(pmm.init(kMap, 1, PhysAddr{2_MiB}, 8_KiB));
    unsigned long taken = 0;
    while (true) {
        PhysAddr const kPage = pmm.allocate_page();
        if (kPage == PhysAddr{}) {
            break;
        }
        ++taken;
    }
    ASSERT_TRUE(taken == 1022);
    ASSERT_TRUE(pmm.free_page_count() == 0);
}

TEST("pmm: ram above 4G joins the ledger up to the cap") {
    cinux::boot::E820Entry const kMap[] = {make_entry(4_GiB, 16_GiB, 1)};
    Pmm                          pmm{};
    ASSERT_TRUE(pmm.init(kMap, 1, PhysAddr{2_MiB}, 8_KiB));
    ASSERT_TRUE(pmm.managed_page_count() == (16_GiB - 4_GiB) / 4_KiB);
    ASSERT_TRUE(pmm.allocate_page() == PhysAddr{4_GiB});
}

TEST("pmm: a map with no usable ram allocates nothing") {
    cinux::boot::E820Entry const kMap[] = {make_entry(0, 128_KiB, 2), make_entry(1_MiB, 4_MiB, 3)};
    Pmm                          pmm{};
    ASSERT_TRUE(pmm.init(kMap, 2, PhysAddr{2_MiB}, 8_KiB));
    ASSERT_TRUE(pmm.managed_page_count() == 0);
    ASSERT_TRUE(pmm.free_page_count() == 0);
    ASSERT_TRUE(pmm.allocate_page() == PhysAddr{});
}

int main() {
    return cinux::test::RunAll();
}
