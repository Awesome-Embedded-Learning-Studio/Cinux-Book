/**
 * @file kernel/test/test_shm.cpp
 * @brief QEMU in-kernel tests for SysV shared memory (F8-M4)
 *
 * Two address spaces map the same segment; a write through one mapping is
 * visible through the other (the whole point of shared memory).  The
 * big-kernel-test environment has no scheduler loop, so each case installs a
 * throwaway Task pointing at a fresh AddressSpace, the same trick
 * test_mmap / test_file_mmap use.
 *
 * SMAP is on in the test kernel, so ring-0 reads/writes of the user-mapped
 * pages go through stac/clac windows (mirrors test_file_mmap.cpp), and each
 * address space is activated (CR3 swap) before its mapping is touched.
 */

#include <stddef.h>
#include <stdint.h>

#include "big_kernel_test.h"
#include "kernel/arch/x86_64/memory_layout.hpp"
#include "kernel/arch/x86_64/paging.hpp"       // write_cr3
#include "kernel/arch/x86_64/user_access.hpp"  // stac / clac
#include "kernel/ipc/shm.hpp"
#include "kernel/mm/address_space.hpp"
#include "kernel/proc/process.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/syscall/sys_shm.hpp"

using cinux::arch::USER_MMAP_BASE;
using cinux::ipc::kIpcCreat;
using cinux::ipc::kIpcExcl;
using cinux::ipc::kIpcPrivate;
using cinux::ipc::kIpcRmid;
using cinux::ipc::kIpcStat;
using cinux::ipc::shmid_ds;
using cinux::mm::AddressSpace;
using cinux::proc::Scheduler;
using cinux::proc::Task;
using cinux::syscall::do_shmctl_kernel;
using cinux::syscall::sys_shmat;
using cinux::syscall::sys_shmctl;
using cinux::syscall::sys_shmget;
using cinux::syscall::sys_shmdt;

namespace {

constexpr uint64_t kPageSize = 4096;

/// RAII: restore the original current task on scope exit.  Switching between
/// two tasks within a test uses Scheduler::set_current directly.
struct CurrentTaskSave {
    Task* prev;
    CurrentTaskSave() : prev(Scheduler::current()) {}
    ~CurrentTaskSave() { Scheduler::set_current(prev); }
};

/// Write an 8-byte little-endian value to a user mapping (SMAP window).
void user_write_u64(uint64_t addr, uint64_t value) {
    cinux::arch::stac();
    *reinterpret_cast<volatile uint64_t*>(addr) = value;
    cinux::arch::clac();
}

/// Read an 8-byte little-endian value from a user mapping (SMAP window).
uint64_t user_read_u64(uint64_t addr) {
    cinux::arch::stac();
    uint64_t v = *reinterpret_cast<volatile uint64_t*>(addr);
    cinux::arch::clac();
    return v;
}

}  // namespace

// ============================================================
// Test 1: two address spaces share one physical page (round-trip)
// ============================================================

namespace test_shm_roundtrip {

void test_two_spaces_share_page() {
    CurrentTaskSave save;

    // Create a 1-page private segment.
    int64_t shmid = sys_shmget(kIpcPrivate, kPageSize, kIpcCreat | 0666, 0, 0, 0);
    TEST_ASSERT_TRUE(shmid >= 0);

    AddressSpace as1;
    AddressSpace as2;
    Task         t1{};
    Task         t2{};
    t1.addr_space = &as1;
    t2.addr_space = &as2;

    // Attach the segment into both address spaces.
    Scheduler::set_current(&t1);
    int64_t virt1 = sys_shmat(shmid, 0, 0, 0, 0, 0);
    TEST_ASSERT_TRUE(virt1 > 0);
    TEST_ASSERT_TRUE(static_cast<uint64_t>(virt1) >= USER_MMAP_BASE);

    Scheduler::set_current(&t2);
    int64_t virt2 = sys_shmat(shmid, 0, 0, 0, 0, 0);
    TEST_ASSERT_TRUE(virt2 > 0);

    // Mechanism proof: both mappings resolve to the SAME physical frame.
    const uint64_t phys1 = as1.translate(static_cast<uint64_t>(virt1));
    const uint64_t phys2 = as2.translate(static_cast<uint64_t>(virt2));
    TEST_ASSERT_TRUE(phys1 != 0);
    TEST_ASSERT_TRUE(phys1 == phys2);

    // End-to-end: write through as1's mapping, read through as2's mapping.
    const uint64_t kMagic = 0x48454c4c4f53484dULL;  // "MSHELLO" (little-endian)
    as1.activate();
    user_write_u64(static_cast<uint64_t>(virt1), kMagic);

    as2.activate();
    uint64_t seen = user_read_u64(static_cast<uint64_t>(virt2));

    // Return to the kernel PML4 before either AddressSpace is torn down.
    cinux::arch::write_cr3(AddressSpace::kernel_pml4());

    TEST_ASSERT_EQ(seen, kMagic);

    // Detach both; the segment retains the page until IPC_RMID.
    Scheduler::set_current(&t1);
    TEST_ASSERT_TRUE(sys_shmdt(static_cast<uint64_t>(virt1), 0, 0, 0, 0, 0) == 0);
    TEST_ASSERT_NULL(as1.vmas().find(static_cast<uint64_t>(virt1)));

    Scheduler::set_current(&t2);
    TEST_ASSERT_TRUE(sys_shmdt(static_cast<uint64_t>(virt2), 0, 0, 0, 0, 0) == 0);

    // IPC_RMID frees the now-unattached segment.
    TEST_ASSERT_TRUE(sys_shmctl(shmid, kIpcRmid, 0, 0, 0, 0) == 0);
}

}  // namespace test_shm_roundtrip

// ============================================================
// Test 2: IPC_STAT reports size and attach count
// ============================================================

namespace test_shm_stat {

void test_ipc_stat_reports_segment() {
    CurrentTaskSave save;

    const uint64_t size  = 3 * kPageSize;
    int64_t        shmid = sys_shmget(kIpcPrivate, size, kIpcCreat | 0666, 0, 0, 0);
    TEST_ASSERT_TRUE(shmid >= 0);

    AddressSpace as;
    Task         t{};
    t.addr_space = &as;
    Scheduler::set_current(&t);

    shmid_ds ds;
    TEST_ASSERT_TRUE(do_shmctl_kernel(shmid, kIpcStat, &ds) == 0);
    TEST_ASSERT_EQ(ds.shm_segsz, size);
    TEST_ASSERT_EQ(ds.shm_nattch, static_cast<uint32_t>(0));

    int64_t virt = sys_shmat(shmid, 0, 0, 0, 0, 0);
    TEST_ASSERT_TRUE(virt > 0);

    TEST_ASSERT_TRUE(do_shmctl_kernel(shmid, kIpcStat, &ds) == 0);
    TEST_ASSERT_EQ(ds.shm_nattch, static_cast<uint32_t>(1));

    TEST_ASSERT_TRUE(sys_shmdt(static_cast<uint64_t>(virt), 0, 0, 0, 0, 0) == 0);
    TEST_ASSERT_TRUE(sys_shmctl(shmid, kIpcRmid, 0, 0, 0, 0) == 0);
}

}  // namespace test_shm_stat

// ============================================================
// Test 3: IPC_RMID detaches the segment; later shmat fails
// ============================================================

namespace test_shm_rmid {

void test_rmid_then_shmat_fails() {
    CurrentTaskSave save;

    int64_t shmid = sys_shmget(kIpcPrivate, kPageSize, kIpcCreat | 0666, 0, 0, 0);
    TEST_ASSERT_TRUE(shmid >= 0);

    AddressSpace as;
    Task         t{};
    t.addr_space = &as;
    Scheduler::set_current(&t);

    // Attach, then mark for removal while still attached: the segment lingers
    // (nattach > 0) and a fresh shmat must be refused.
    int64_t virt = sys_shmat(shmid, 0, 0, 0, 0, 0);
    TEST_ASSERT_TRUE(virt > 0);

    TEST_ASSERT_TRUE(sys_shmctl(shmid, kIpcRmid, 0, 0, 0, 0) == 0);
    TEST_ASSERT_TRUE(sys_shmat(shmid, 0, 0, 0, 0, 0) < 0);  // marked -> EINVAL

    // The last detach reclaims the pages (registry hands them to the syscall).
    TEST_ASSERT_TRUE(sys_shmdt(static_cast<uint64_t>(virt), 0, 0, 0, 0, 0) == 0);

    // An unattached RMID frees immediately.
    int64_t shmid2 = sys_shmget(kIpcPrivate, kPageSize, kIpcCreat | 0666, 0, 0, 0);
    TEST_ASSERT_TRUE(shmid2 >= 0);
    TEST_ASSERT_TRUE(sys_shmctl(shmid2, kIpcRmid, 0, 0, 0, 0) == 0);
    TEST_ASSERT_TRUE(sys_shmat(shmid2, 0, 0, 0, 0, 0) < 0);
}

}  // namespace test_shm_rmid

// ============================================================
// Test 4: named-key create / reopen / EXCL / lookup-fail
// ============================================================

namespace test_shm_named_key {

void test_named_key_semantics() {
    CurrentTaskSave save;
    const uint64_t  key = 0x53484D31;  // "SHM1"

    // First create with IPC_CREAT.
    int64_t shmid = sys_shmget(key, kPageSize, kIpcCreat | 0666, 0, 0, 0);
    TEST_ASSERT_TRUE(shmid >= 0);

    // Reopen without EXCL -> same shmid.
    int64_t again = sys_shmget(key, 2 * kPageSize, kIpcCreat | 0666, 0, 0, 0);
    TEST_ASSERT_EQ(again, shmid);

    // EXCL on an existing key -> EEXIST (-errno < 0).
    TEST_ASSERT_TRUE(sys_shmget(key, kPageSize, kIpcCreat | kIpcExcl | 0666, 0, 0, 0) < 0);

    // A distinct unknown key without IPC_CREAT -> ENOENT (< 0).
    TEST_ASSERT_TRUE(sys_shmget(key + 1, kPageSize, 0, 0, 0, 0) < 0);

    TEST_ASSERT_TRUE(sys_shmctl(shmid, kIpcRmid, 0, 0, 0, 0) == 0);
}

}  // namespace test_shm_named_key

// ============================================================
// Test 5: invalid arguments are rejected
// ============================================================

namespace test_shm_invalid {

void test_invalid_args() {
    CurrentTaskSave save;

    // size == 0 -> EINVAL.
    TEST_ASSERT_TRUE(sys_shmget(kIpcPrivate, 0, kIpcCreat | 0666, 0, 0, 0) < 0);

    AddressSpace as;
    Task         t{};
    t.addr_space = &as;
    Scheduler::set_current(&t);

    // shmat on a bogus shmid -> EINVAL.
    TEST_ASSERT_TRUE(sys_shmat(0xDEAD, 0, 0, 0, 0, 0) < 0);

    // shmdt of an address with no mapping -> EINVAL.
    TEST_ASSERT_TRUE(sys_shmdt(USER_MMAP_BASE, 0, 0, 0, 0, 0) < 0);

    // shmctl IPC_STAT on a bogus id -> failure (< 0).
    shmid_ds ds;
    TEST_ASSERT_TRUE(do_shmctl_kernel(0xDEAD, kIpcStat, &ds) < 0);
}

}  // namespace test_shm_invalid

// ============================================================
// Test 6: adjacent attachments in one address space (VMA-merge regression)
// ============================================================

namespace test_shm_adjacent {

// Two same-flags SHM mappings placed back-to-back coalesce into one VMA.
// shmdt of the first must still leave the second intact -- it derives the
// teardown length from the segment, not the (merged) VMA's [start,end).
void test_adjacent_detach_preserves_peer() {
    CurrentTaskSave save;

    int64_t s1 = sys_shmget(kIpcPrivate, kPageSize, kIpcCreat | 0666, 0, 0, 0);
    int64_t s2 = sys_shmget(kIpcPrivate, kPageSize, kIpcCreat | 0666, 0, 0, 0);
    TEST_ASSERT_TRUE(s1 >= 0);
    TEST_ASSERT_TRUE(s2 >= 0);

    AddressSpace as;
    Task         t{};
    t.addr_space = &as;
    Scheduler::set_current(&t);

    // Place them adjacently via the fixed-address shmat path.
    const uint64_t base = USER_MMAP_BASE;
    const uint64_t a1   = base;
    const uint64_t a2   = base + kPageSize;
    TEST_ASSERT_TRUE(sys_shmat(s1, a1, 0, 0, 0, 0) == static_cast<int64_t>(a1));
    TEST_ASSERT_TRUE(sys_shmat(s2, a2, 0, 0, 0, 0) == static_cast<int64_t>(a2));

    // Distinct content per page, written through the live address space.
    const uint64_t kOne = 0x1111111111111111ULL;
    const uint64_t kTwo = 0x2222222222222222ULL;
    as.activate();
    user_write_u64(a1, kOne);
    user_write_u64(a2, kTwo);
    cinux::arch::write_cr3(AddressSpace::kernel_pml4());

    // Detach the first segment only.
    TEST_ASSERT_TRUE(sys_shmdt(a1, 0, 0, 0, 0, 0) == 0);
    TEST_ASSERT_TRUE(as.translate(a1) == 0);  // first page gone
    TEST_ASSERT_TRUE(as.translate(a2) != 0);  // second page still mapped

    // The peer must still read its own value, not be clobbered.
    as.activate();
    uint64_t got = user_read_u64(a2);
    cinux::arch::write_cr3(AddressSpace::kernel_pml4());
    TEST_ASSERT_EQ(got, kTwo);

    TEST_ASSERT_TRUE(sys_shmdt(a2, 0, 0, 0, 0, 0) == 0);
    TEST_ASSERT_TRUE(sys_shmctl(s1, kIpcRmid, 0, 0, 0, 0) == 0);
    TEST_ASSERT_TRUE(sys_shmctl(s2, kIpcRmid, 0, 0, 0, 0) == 0);
}

}  // namespace test_shm_adjacent

// ============================================================
// Entry point
// ============================================================

extern "C" void run_shm_tests() {
    TEST_SECTION("SHM Tests (F8-M4)");

    RUN_TEST(test_shm_roundtrip::test_two_spaces_share_page);
    RUN_TEST(test_shm_stat::test_ipc_stat_reports_segment);
    RUN_TEST(test_shm_rmid::test_rmid_then_shmat_fails);
    RUN_TEST(test_shm_named_key::test_named_key_semantics);
    RUN_TEST(test_shm_invalid::test_invalid_args);
    RUN_TEST(test_shm_adjacent::test_adjacent_detach_preserves_peer);

    TEST_SUMMARY();
}
