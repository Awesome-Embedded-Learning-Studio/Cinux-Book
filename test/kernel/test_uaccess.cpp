#include "cinux/own.hpp"
#include "cinux/ptr.hpp"
#include "cinux/shared.hpp"
#include "framework_kernel.hpp"
#include "kernel/arch/x86_64/cpuid.hpp"
#include "kernel/arch/x86_64/irq_guard.hpp"
#include "kernel/arch/x86_64/page.hpp"
#include "kernel/arch/x86_64/protections.hpp"
#include "kernel/arch/x86_64/registers.hpp"
#include "kernel/boot/print.hpp"
#include "kernel/mm/address_space.hpp"
#include "kernel/mm/cow.hpp"
#include "kernel/mm/layout.hpp"
#include "kernel/mm/vma.hpp"
#include "kernel/proc/fault_resolve.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/task.hpp"
#include "kernel/syscall/uaccess.hpp"
#include "test_assert.hpp"
#include "test_case.hpp"  // NOLINT(misc-include-cleaner) consumed by the TEST() macro body

namespace {

/// A hole below the user limit with no VMA and no mapping in any test
/// world; the v1 judgement seat, carried over.
constexpr unsigned long long kHoleAddress = 0x7000'0000'0000ULL;

constexpr unsigned long      kCopyAddress = 0x6000'0000UL;
constexpr unsigned long long kPayload     = 0x114514'ABCDEFULL;

bool register_copy_range(bool writable, cinux::mm::VmaKind kind = cinux::mm::VmaKind::kAnonymous) {
    cinux::mm::VmaRegion range{};
    range.start    = kCopyAddress;
    range.end      = kCopyAddress + (2 * cinux::arch::page::kSize);
    range.kind     = kind;
    range.readable = true;
    range.writable = writable;
    return cinux::proc::Scheduler::self().current()->vmas->register_range(range).ok();
}

void run_in_world(void (*body)()) {
    const cinux::arch::IrqGuard kGuard;
    const auto                  kSpace  = cinux::base::MakeShared<cinux::mm::AddressSpace>();
    const auto                  kRanges = cinux::base::MakeShared<cinux::mm::VmaStore>();
    ASSERT_TRUE(kSpace.holds() && kRanges.holds());
    ASSERT_TRUE(kSpace->init());
    auto* const kTask = cinux::proc::TaskBuilder{}.set_entry(body).set_name("uaccess").build();
    ASSERT_TRUE(kTask != nullptr);
    kTask->space          = kSpace;
    kTask->vmas           = kRanges;
    kTask->user_root      = kSpace->root();
    const auto kBeforeCr4 = cinux::arch::ReadCr4();
    cinux::arch::EnableCoreProtections();
    cinux::proc::InstallFaultResolver();
    cinux::proc::Scheduler::self().seat(*kTask);
    cinux::proc::Scheduler::self().run_until_done();
    cinux::arch::WriteCr4(kBeforeCr4);
}

void read_untouched_page() {
    ASSERT_TRUE(register_copy_range(true));
    unsigned long long seen = kPayload;
    ASSERT_TRUE(cinux::syscall::uaccess::CopyFromUser(&seen, kCopyAddress, sizeof(seen)));
    ASSERT_EQ(seen, 0ULL);
    ASSERT_TRUE(cinux::proc::Scheduler::self().current()->space->translate(kCopyAddress) != 0);
}

void write_across_pages() {
    ASSERT_TRUE(register_copy_range(true));
    constexpr unsigned long  kStart    = kCopyAddress + cinux::arch::page::kSize - 8;
    const unsigned long long kWords[2] = {kPayload, ~kPayload};
    ASSERT_TRUE(cinux::syscall::uaccess::CopyToUser(kStart, kWords, sizeof(kWords)));
    unsigned long long seen[2] = {};
    ASSERT_TRUE(cinux::syscall::uaccess::CopyFromUser(seen, kStart, sizeof(seen)));
    ASSERT_EQ(seen[0], kWords[0]);
    ASSERT_EQ(seen[1], kWords[1]);
}

void write_shared_page() {
    ASSERT_TRUE(register_copy_range(true));
    const auto& space = *cinux::proc::Scheduler::self().current()->space;
    ASSERT_TRUE(cinux::syscall::uaccess::CopyToUser(kCopyAddress, &kPayload, sizeof(kPayload)));
    const cinux::base::Own<cinux::mm::AddressSpace> kChild(cinux::mm::cow::CloneSharedSpace(space));
    ASSERT_TRUE(kChild.holds());
    const auto                   kSharedFrame = kChild->translate(kCopyAddress);
    constexpr unsigned long long kChanged     = ~kPayload;
    ASSERT_TRUE(cinux::syscall::uaccess::CopyToUser(kCopyAddress, &kChanged, sizeof(kChanged)));
    ASSERT_TRUE(space.translate(kCopyAddress) != kSharedFrame);
    unsigned long long seen = 0;
    ASSERT_TRUE(cinux::syscall::uaccess::CopyFromUser(&seen, kCopyAddress, sizeof(seen)));
    ASSERT_EQ(seen, kChanged);
    const auto* const kOriginal =
        cinux::base::PtrAt<const unsigned long long>(cinux::mm::DirectMapVirt(kSharedFrame));
    ASSERT_EQ(*kOriginal, kPayload);
}

void check_write_refused() {
    ASSERT_FALSE(cinux::syscall::uaccess::CopyToUser(kCopyAddress, &kPayload, sizeof(kPayload)));
}

void write_readonly_pages() {
    ASSERT_TRUE(register_copy_range(false));
    const auto& self = *cinux::proc::Scheduler::self().current();
    check_write_refused();
    ASSERT_EQ(self.space->translate(kCopyAddress), 0UL);
    unsigned long long seen = kPayload;
    ASSERT_TRUE(cinux::syscall::uaccess::CopyFromUser(&seen, kCopyAddress, sizeof(seen)));
    ASSERT_EQ(seen, 0ULL);
    check_write_refused();
    const cinux::base::Own<cinux::mm::AddressSpace> kChild(
        cinux::mm::cow::CloneSharedSpace(*self.space));
    ASSERT_TRUE(kChild.holds());
    check_write_refused();
    ASSERT_EQ(self.space->translate(kCopyAddress), kChild->translate(kCopyAddress));
}

void copy_stack_guard() {
    ASSERT_TRUE(register_copy_range(true, cinux::mm::VmaKind::kStack));
    unsigned long long seen = 0;
    ASSERT_FALSE(cinux::syscall::uaccess::CopyFromUser(&seen, kCopyAddress, sizeof(seen)));
    ASSERT_FALSE(cinux::syscall::uaccess::CopyToUser(kCopyAddress, &kPayload, sizeof(kPayload)));
    ASSERT_TRUE(cinux::syscall::uaccess::CopyToUser(kCopyAddress + cinux::arch::page::kSize,
                                                    &kPayload, sizeof(kPayload)));
}

void copy_with_closed_window() {
    ASSERT_TRUE(register_copy_range(true));
    const auto& space = *cinux::proc::Scheduler::self().current()->space;
    ASSERT_TRUE(cinux::syscall::uaccess::CopyToUser(kCopyAddress, &kPayload, sizeof(kPayload)));
    const cinux::base::Own<cinux::mm::AddressSpace> kChild(cinux::mm::cow::CloneSharedSpace(space));
    ASSERT_TRUE(kChild.holds());
    const auto kSharedFrame = space.translate(kCopyAddress);
    cinux::syscall::uaccess::ArmWindows(false);
    const bool kMoved =
        cinux::syscall::uaccess::CopyToUser(kCopyAddress, &kPayload, sizeof(kPayload));
    cinux::syscall::uaccess::ArmWindows(true);
    ASSERT_FALSE(kMoved);
    ASSERT_EQ(space.translate(kCopyAddress), kSharedFrame);
    unsigned long long seen = 0;
    ASSERT_TRUE(cinux::syscall::uaccess::CopyFromUser(&seen, kCopyAddress, sizeof(seen)));
    ASSERT_EQ(seen, kPayload);
}

}  // namespace

TEST("uaccess: a copy from an unmapped hole bounces, not dies") {
    unsigned long long hold = 0;
    const bool kMoved = cinux::syscall::uaccess::CopyFromUser(&hold, kHoleAddress, sizeof(hold));
    ASSERT_FALSE(kMoved);
}

TEST("uaccess: a copy to an unmapped hole bounces the same way") {
    const bool kMoved =
        cinux::syscall::uaccess::CopyToUser(kHoleAddress, &kPayload, sizeof(kPayload));
    ASSERT_FALSE(kMoved);
    cinux::print::Println("[uaccess] both bounces returned, the kernel never died");
}

TEST("uaccess: reading an untouched promised page fills and retries the copy") {
    run_in_world(read_untouched_page);
}

TEST("uaccess: a write crossing two untouched pages fills both and resumes") {
    run_in_world(write_across_pages);
}

TEST("uaccess: a post-fork write copies a shared page and preserves the child") {
    run_in_world(write_shared_page);
}

TEST("uaccess: read-only promises reject writes before and after residency and fork") {
    run_in_world(write_readonly_pages);
}

TEST("uaccess: the stack guard refuses copies while the next page fills") {
    run_in_world(copy_stack_guard);
}

TEST("uaccess: a closed SMAP window cannot promote a writable CoW promise") {
    if (!cinux::arch::HasSmap()) {
        return;
    }
    run_in_world(copy_with_closed_window);
}
