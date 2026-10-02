#include <stdint.h>

#include <initializer_list>

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

cinux::boot::BootInfo make_info(std::initializer_list<cinux::boot::E820Entry> entries,
                                unsigned long kernel_base, unsigned long kernel_bytes) {
    cinux::boot::BootInfo info{};
    uint32_t              index = 0;
    for (auto const& entry : entries) {
        info.e820[index++] = entry;
    }
    info.e820_count      = index;
    info.kernel_paddr    = kernel_base;
    info.kernel_mem_size = kernel_bytes;
    return info;
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
    Pmm pmm{};
    ASSERT_TRUE(pmm.init(make_info(
        {make_entry(0, 128_KiB, 1), make_entry(128_KiB, 896_KiB, 2), make_entry(1_MiB, 4_MiB, 1)},
        2_MiB, 8_KiB)));
    ASSERT_TRUE(pmm.managed_page_count() == 1024);
    ASSERT_TRUE(pmm.free_page_count() == 1022);
    ASSERT_TRUE(pmm.allocate_page() == PhysAddr{1_MiB});
}

TEST("pmm: order runs respect kernel holes") {
    Pmm pmm{};
    ASSERT_TRUE(pmm.init(make_info({make_entry(1_MiB, 4_MiB, 1)}, 2_MiB, 8_KiB)));
    unsigned long const kAfterKernel = 2_MiB + 8_KiB;
    ASSERT_TRUE(pmm.allocate_pages(9) == PhysAddr{kAfterKernel});
    ASSERT_TRUE(pmm.free_page_count() == 1022 - 512);
    ASSERT_TRUE(pmm.allocate_pages(9) == PhysAddr{});
    pmm.free_pages(PhysAddr{kAfterKernel}, 9);
    ASSERT_TRUE(pmm.free_page_count() == 1022);
}

TEST("pmm: order bounds are refused") {
    Pmm pmm{};
    ASSERT_TRUE(pmm.init(make_info({make_entry(1_MiB, 4_MiB, 1)}, 2_MiB, 8_KiB)));
    ASSERT_TRUE(pmm.allocate_pages(-1) == PhysAddr{});
    ASSERT_TRUE(pmm.allocate_pages(10) == PhysAddr{});
    ASSERT_TRUE(pmm.free_page_count() == 1022);
}

TEST("pmm: every free page is reachable exactly once") {
    Pmm pmm{};
    ASSERT_TRUE(pmm.init(make_info({make_entry(1_MiB, 4_MiB, 1)}, 2_MiB, 8_KiB)));
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
    Pmm pmm{};
    ASSERT_TRUE(pmm.init(make_info({make_entry(4_GiB, 16_GiB, 1)}, 2_MiB, 8_KiB)));
    ASSERT_TRUE(pmm.managed_page_count() == (16_GiB - 4_GiB) / 4_KiB);
    ASSERT_TRUE(pmm.allocate_page() == PhysAddr{4_GiB});
}

TEST("pmm: a map with no usable ram allocates nothing") {
    Pmm pmm{};
    ASSERT_TRUE(pmm.init(
        make_info({make_entry(0, 128_KiB, 2), make_entry(1_MiB, 4_MiB, 3)}, 2_MiB, 8_KiB)));
    ASSERT_TRUE(pmm.managed_page_count() == 0);
    ASSERT_TRUE(pmm.free_page_count() == 0);
    ASSERT_TRUE(pmm.allocate_page() == PhysAddr{});
}

int main() {
    return cinux::test::RunAll();
}
