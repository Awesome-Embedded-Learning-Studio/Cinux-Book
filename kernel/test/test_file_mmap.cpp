/**
 * @file kernel/test/test_file_mmap.cpp
 * @brief F2-M4 batch 3: file-backed mmap demand-read end-to-end on real ext2
 *
 *   Test 1 -- g_page_cache.get_page() on a real ext2 inode reads the file's
 *             actual bytes (cache -> Ext2FileOps::read -> AHCI).
 *   Test 2 -- a file-backed user VMA demand-paged on #PF: after activating the
 *             address space (so the PF maps into our PML4), reading the user
 *             address triggers #PF, handle_pf's file path fills it from the
 *             cache, and the byte matches the file content.
 *
 * AHCI/ext2 setup mirrors test_ext2_inode_ops.cpp.  SMAP is not yet enabled
 * (F9), so ring-0 test code may read a user-mapped page directly.
 */

#include <stddef.h>
#include <stdint.h>

#include "big_kernel_test.h"
#include "kernel/arch/x86_64/memory_layout.hpp"
#include "kernel/arch/x86_64/paging.hpp"
#include "kernel/arch/x86_64/paging_config.hpp"
#include "kernel/drivers/ahci/ahci.hpp"
#include "kernel/drivers/ahci/ahci_block_device.hpp"
#include "kernel/drivers/pci/pci.hpp"
#include "kernel/drivers/pit/pit.hpp"
#include "kernel/fs/ext2/ext2.hpp"
#include "kernel/fs/inode.hpp"
#include "kernel/mm/address_space.hpp"
#include "kernel/mm/page_cache.hpp"
#include "kernel/mm/vma.hpp"
#include "kernel/proc/process.hpp"
#include "kernel/proc/scheduler.hpp"

using cinux::arch::PAGE_SIZE;
using cinux::arch::USER_MMAP_END;
using cinux::drivers::ahci::AHCIBlockDevice;
using cinux::drivers::ahci::AHCI;
using cinux::drivers::pci::PCI;
using cinux::drivers::pci::PCIDevice;
using cinux::fs::Ext2;
using cinux::fs::Inode;
using cinux::mm::AddressSpace;
using cinux::mm::VMA;
using cinux::mm::VmaFlags;
using cinux::proc::Task;

namespace {

// ============================================================
// AHCI + Ext2 fixture (mirrors test_ext2_inode_ops.cpp)
// ============================================================

struct AhciExt2Pair {
    AHCI*            ahci;
    Ext2*            ext2;
    AHCIBlockDevice* blk_dev;
};

AhciExt2Pair setup_ext2() {
    AhciExt2Pair result{nullptr, nullptr, nullptr};

    PCI pci;
    pci.init();

    PCIDevice ahci_dev{};
    if (!pci.find_ahci(ahci_dev)) {
        return result;
    }

    result.ahci = new AHCI();
    result.ahci->init(ahci_dev);
    if (result.ahci->hba_mem() == nullptr) {
        return result;
    }

    auto blk       = AHCIBlockDevice::create(*result.ahci, 1);
    result.blk_dev = blk.ok() ? new AHCIBlockDevice(std::move(blk.value())) : nullptr;
    result.ext2    = new Ext2(result.blk_dev);
    ASSERT_OK(result.ext2->mount());

    return result;
}

void teardown_ext2(AhciExt2Pair& pair) {
    delete pair.ext2;
    delete pair.blk_dev;
    delete pair.ahci;
    pair.ext2    = nullptr;
    pair.blk_dev = nullptr;
    pair.ahci    = nullptr;
}

static uint32_t g_name_seq = 0;

void gen_name(char* buf, uint32_t buf_len, const char* prefix) {
    uint32_t seed = static_cast<uint32_t>(cinux::drivers::PIT::get_ticks() ^ (++g_name_seq));
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    uint32_t off = 0;
    while (prefix[off] && off < buf_len - 9) {
        buf[off] = prefix[off];
        ++off;
    }
    buf[off++]       = '_';
    const char hex[] = "0123456789abcdef";
    for (int d = 6; d >= 0 && off < buf_len - 1; --d)
        buf[off++] = hex[(seed >> (d * 4)) & 0xf];
    buf[off] = '\0';
}

uint32_t name_len(const char* s) {
    uint32_t n = 0;
    while (s[n]) {
        ++n;
    }
    return n;
}

/// RAII: install @p task as current, restore the previous on destruction.
struct CurrentTaskGuard {
    cinux::proc::Task* prev;
    explicit CurrentTaskGuard(cinux::proc::Task* task) : prev(cinux::proc::Scheduler::current()) {
        cinux::proc::Scheduler::set_current(task);
    }
    ~CurrentTaskGuard() { cinux::proc::Scheduler::set_current(prev); }
};

/// Deterministic content: byte i = (i*7+3) & 0xFF.
void fill_pattern(uint8_t* buf, uint64_t len) {
    for (uint64_t i = 0; i < len; i++) {
        buf[i] = static_cast<uint8_t>((i * 7u + 3u) & 0xFFu);
    }
}

}  // namespace

// ============================================================
// Test 1: page cache reads a real ext2 file's content
// ============================================================

namespace test_fm_cache {

void test_cache_reads_real_file() {
    auto pair = setup_ext2();
    TEST_ASSERT_NOT_NULL(pair.ext2);
    TEST_ASSERT_TRUE(pair.ext2->is_mounted());

    char name[32];
    gen_name(name, 32, "fmc");
    Inode* ino = pair.ext2->create(2, name, name_len(name));
    TEST_ASSERT_NOT_NULL(ino);
    TEST_ASSERT_NOT_NULL(ino->ops);

    // Write two pages of known content through the inode.
    static uint8_t wbuf[2 * PAGE_SIZE];
    fill_pattern(wbuf, sizeof(wbuf));
    int64_t written = write_or_neg1(ino, 0, wbuf, sizeof(wbuf));
    TEST_ASSERT_EQ(written, static_cast<int64_t>(sizeof(wbuf)));

    // The cache must serve the file's real bytes (cache -> ext2 -> AHCI).
    auto r0 = cinux::mm::g_page_cache.get_page(ino, 0);
    auto r1 = cinux::mm::g_page_cache.get_page(ino, PAGE_SIZE);
    TEST_ASSERT_TRUE(r0.ok());
    TEST_ASSERT_TRUE(r1.ok());

    auto* base0 = reinterpret_cast<uint8_t*>(r0.value()->virt);
    auto* base1 = reinterpret_cast<uint8_t*>(r1.value()->virt);
    for (uint64_t i = 0; i < PAGE_SIZE; i += 257) {
        TEST_ASSERT_EQ(base0[i], wbuf[i]);
        TEST_ASSERT_EQ(base1[i], wbuf[PAGE_SIZE + i]);
    }

    // A second get_page on the same key is a cache hit (no new page).
    TEST_ASSERT_EQ(cinux::mm::g_page_cache.cached_pages(), static_cast<size_t>(2));
    auto r0b = cinux::mm::g_page_cache.get_page(ino, 0);
    TEST_ASSERT_TRUE(r0b.ok());
    TEST_ASSERT_TRUE(r0b.value() == r0.value());

    pair.ext2->unlink(2, name, name_len(name));
    teardown_ext2(pair);
}

}  // namespace test_fm_cache

// ============================================================
// Test 2: a file-backed user VMA demand-reads on #PF (handle_pf wiring)
// ============================================================

namespace test_fm_pf {

void test_pf_reads_file_content() {
    auto pair = setup_ext2();
    TEST_ASSERT_NOT_NULL(pair.ext2);
    TEST_ASSERT_TRUE(pair.ext2->is_mounted());

    char name[32];
    gen_name(name, 32, "fmp");
    Inode* ino = pair.ext2->create(2, name, name_len(name));
    TEST_ASSERT_NOT_NULL(ino);

    static uint8_t wbuf[PAGE_SIZE];
    fill_pattern(wbuf, sizeof(wbuf));
    int64_t written = write_or_neg1(ino, 0, wbuf, PAGE_SIZE);
    TEST_ASSERT_EQ(written, static_cast<int64_t>(PAGE_SIZE));

    // Install a current task and record a file-backed user VMA.  A high
    // address avoids colliding with other tests' find_free_area allocations.
    AddressSpace as;
    Task         tmp{};
    tmp.addr_space = &as;
    CurrentTaskGuard guard(&tmp);

    const uint64_t map_addr = USER_MMAP_END - PAGE_SIZE;
    auto           ir       = as.vmas().insert(map_addr, map_addr + PAGE_SIZE, VmaFlags::Read);
    TEST_ASSERT_TRUE(ir.ok());
    VMA* v = as.vmas().find(map_addr);
    TEST_ASSERT_NOT_NULL(v);
    v->backing     = ino;
    v->file_offset = 0;

    // Switch to this address space so handle_pf maps the demand-paged user
    // page into OUR PML4.  The kernel half is mirrored, so execution continues.
    as.activate();

    // Read through the user mapping.  The first access is not present -> #PF
    // -> handle_pf's file path fills the page from the cache and maps it.
    // F9 batch 4 (SMAP): this test runs in ring 0, so directly reading the
    // user-mapped page needs STAC to bypass SMAP (same way a syscall handler
    // gets it via the entry STAC).  CLAC restores the guard after.
    __asm__ volatile("stac");
    volatile uint8_t first = *reinterpret_cast<volatile uint8_t*>(map_addr);
    volatile uint8_t mid   = *reinterpret_cast<volatile uint8_t*>(map_addr + PAGE_SIZE / 2);
    volatile uint8_t last  = *reinterpret_cast<volatile uint8_t*>(map_addr + PAGE_SIZE - 1);
    __asm__ volatile("clac");

    // Return to the kernel PML4 before `as` is destroyed (its page tables are
    // freed on destruction).
    cinux::arch::write_cr3(cinux::mm::AddressSpace::kernel_pml4());

    TEST_ASSERT_EQ(first, wbuf[0]);
    TEST_ASSERT_EQ(mid, wbuf[PAGE_SIZE / 2]);
    TEST_ASSERT_EQ(last, wbuf[PAGE_SIZE - 1]);

    pair.ext2->unlink(2, name, name_len(name));
    teardown_ext2(pair);
}

}  // namespace test_fm_pf

// ============================================================
// Entry point
// ============================================================

extern "C" void run_file_mmap_tests() {
    TEST_SECTION("File mmap Tests (F2-M4-3)");

    RUN_TEST(test_fm_cache::test_cache_reads_real_file);
    RUN_TEST(test_fm_pf::test_pf_reads_file_content);

    TEST_SUMMARY();
}
