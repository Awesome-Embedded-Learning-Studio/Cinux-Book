/**
 * @file kernel/test/test_user_ptr.cpp
 * @brief QEMU in-kernel tests for UserPtr<T> + SMAP user accessors (F-QA Q4a-2 / SMAP M0)
 *
 * UserPtr is a zero-overhead marker for user-space pointers (sparse __user
 * semantics). These tests exercise its type-level contract: it holds the
 * pointer unchanged, converts back to raw (drop-in via operator T()), supports
 * member access / dereference for typed pointers, allows nullptr (unlike
 * NotNull), and adds no runtime validation (that stays with validate_user_ptr).
 *
 * The SMAP section (M0) tests cinux::user::access_ok as a pure range check and
 * the access_ok rejection path of copy_to/from_user/put_user/get_user. The
 * kernel high-half addresses used here are rejected before any copy runs, so no
 * user page is touched and no fault can occur. The happy-path copy round-trip
 * is exercised for real in P0/P1 by syscalls operating on genuine user pointers.
 */

#include <stddef.h>
#include <stdint.h>

#include "big_kernel_test.h"
#include "kernel/arch/x86_64/user_access.hpp"  // cinux::user::access_ok / copy_*/put/get
#include "kernel/lib/user_ptr.hpp"

using cinux::lib::UserPtr;

namespace {

struct Sample {
    int value;
    int doubled() const { return value * 2; }
};

// A sink taking a raw pointer — proves UserPtr is a drop-in via operator T().
int read_via_raw(const int* p) {
    return *p;
}

}  // namespace

namespace test_user_ptr {

void test_default_construct_is_null() {
    UserPtr<int*> up;
    TEST_ASSERT_NULL(up.get());
}

void test_holds_constructed_pointer() {
    int           x = 42;
    UserPtr<int*> up(&x);
    TEST_ASSERT_EQ(up.get(), &x);
}

void test_implicit_conversion_drop_in() {
    int           x = 7;
    UserPtr<int*> up(&x);
    // operator T() passes straight to a raw-pointer sink — no .get() needed.
    TEST_ASSERT_EQ(read_via_raw(up), 7);
}

void test_member_access_arrow() {
    Sample           s{99};
    UserPtr<Sample*> up(&s);
    TEST_ASSERT_EQ(up->value, 99);
    TEST_ASSERT_EQ(up->doubled(), 198);
}

void test_dereference_star() {
    int           x = 1234;
    UserPtr<int*> up(&x);
    TEST_ASSERT_EQ(*up, 1234);
    *up = 5678;  // writable through the marker
    TEST_ASSERT_EQ(x, 5678);
}

void test_nullptr_allowed_unlike_not_null() {
    // A user pointer may legitimately be NULL (syscall -> -EFAULT); UserPtr
    // does NOT trap on null construction (unlike NotNull, which asserts).
    UserPtr<int*> up(nullptr);
    TEST_ASSERT_NULL(up.get());
}

void test_const_char_pointer() {
    const char*          msg = "hello";
    UserPtr<const char*> up(msg);
    TEST_ASSERT_EQ(up.get()[0], 'h');
    TEST_ASSERT_EQ(up.get()[4], 'o');
}

// ============================================================
// SMAP accessor tests (M0) — cinux::user::access_ok + rejection path
// ============================================================

// access_ok is a pure range check (no memory access), so literal addresses are
// safe to test directly. User half = bit47 = 0, i.e. 0..0x00007FFFFFFFFFFF.
void test_access_ok_accepts_user_range() {
    TEST_ASSERT_TRUE(cinux::user::access_ok(reinterpret_cast<void*>(0x400000ULL), 0x1000));
    TEST_ASSERT_TRUE(cinux::user::access_ok(reinterpret_cast<void*>(0x1000ULL), 1));
    // Last full user page: [0x7FFFEFFF_F000, 0x7FFFF000_0000) stays in user half.
    TEST_ASSERT_TRUE(cinux::user::access_ok(reinterpret_cast<void*>(0x7FFFEFFFF000ULL), 0x1000));
    TEST_ASSERT_TRUE(cinux::user::access_ok(reinterpret_cast<void*>(0x400000ULL), 0));  // size 0 ok
}

void test_access_ok_rejects_kernel_addr() {
    // Kernel high-half canonical (bit47 = 1) must be rejected.
    TEST_ASSERT_FALSE(cinux::user::access_ok(reinterpret_cast<void*>(0xFFFF800000000000ULL), 8));
    TEST_ASSERT_FALSE(cinux::user::access_ok(reinterpret_cast<void*>(0xFFFFFFFF80100000ULL), 8));
}

void test_access_ok_rejects_null() {
    TEST_ASSERT_FALSE(cinux::user::access_ok(nullptr, 8));
    TEST_ASSERT_FALSE(cinux::user::access_ok(nullptr, 0));
}

void test_access_ok_rejects_wraparound() {
    // Starts in user half but crosses the 2^47 boundary into kernel half.
    TEST_ASSERT_FALSE(cinux::user::access_ok(reinterpret_cast<void*>(0x7FFFFFFFFFFFULL), 2));
    // addr + size wraps past 2^64.
    TEST_ASSERT_FALSE(cinux::user::access_ok(reinterpret_cast<void*>(0xFFFFFFFFFFFFFFFFULL), 2));
}

// copy/put/get with a kernel-range pointer must short-circuit at access_ok and
// return false WITHOUT touching memory (no stac, no copy, no fault). Verifies
// the guard fires before the SMAP window opens.
void test_copy_rejects_kernel_addr() {
    char        ksrc[4]      = {1, 2, 3, 4};
    char        kdst[4]      = {0, 0, 0, 0};
    void*       kernel_addr  = reinterpret_cast<void*>(0xFFFF800000000000ULL);
    const void* kernel_caddr = reinterpret_cast<const void*>(0xFFFF800000000000ULL);

    TEST_ASSERT_FALSE(cinux::user::copy_to_user(kernel_addr, ksrc, 4));
    TEST_ASSERT_FALSE(cinux::user::copy_from_user(kdst, kernel_caddr, 4));
    TEST_ASSERT_FALSE(cinux::user::put_user(42, reinterpret_cast<int*>(kernel_addr)));
    int got = 0;
    TEST_ASSERT_FALSE(cinux::user::get_user(&got, reinterpret_cast<const int*>(kernel_caddr)));

    // kdst was never written (copy never ran).
    TEST_ASSERT_EQ(kdst[0], 0);
    TEST_ASSERT_EQ(kdst[3], 0);
}

}  // namespace test_user_ptr

// F-EXTABLE B4: a genuine mid-accessor #PF must be caught by the __ex_table
// fixup and return false (caller -EFAULT), NOT panic. Unlike the kernel-addr
// tests above (rejected by access_ok before any copy runs), these pass
// access_ok (valid user-half address) but hit an unmapped page inside the rep
// movsb, so the fault is real and only the fixup saves us. If the fixup ever
// fails to fire, the test kernel panics (cli;hlt) and run-kernel-test-all times
// out -- the "panic == fail" contract is structural.
namespace test_extable {

void test_copy_from_unmapped_returns_false() {
    uint8_t buf[8]        = {0};
    void*   unmapped_user = reinterpret_cast<void*>(0x7000000000ULL);
    TEST_ASSERT_FALSE(cinux::user::copy_from_user(buf, unmapped_user, sizeof(buf)));
}

void test_copy_to_unmapped_returns_false() {
    uint8_t src[8]        = {1, 2, 3, 4, 5, 6, 7, 8};
    void*   unmapped_user = reinterpret_cast<void*>(0x7000000000ULL);
    TEST_ASSERT_FALSE(cinux::user::copy_to_user(unmapped_user, src, sizeof(src)));
}

}  // namespace test_extable

extern "C" void run_user_ptr_tests() {
    TEST_SECTION("UserPtr (F-QA Q4a-2)");
    RUN_TEST(test_user_ptr::test_default_construct_is_null);
    RUN_TEST(test_user_ptr::test_holds_constructed_pointer);
    RUN_TEST(test_user_ptr::test_implicit_conversion_drop_in);
    RUN_TEST(test_user_ptr::test_member_access_arrow);
    RUN_TEST(test_user_ptr::test_dereference_star);
    RUN_TEST(test_user_ptr::test_nullptr_allowed_unlike_not_null);
    RUN_TEST(test_user_ptr::test_const_char_pointer);

    TEST_SECTION("SMAP user accessors (M0)");
    RUN_TEST(test_user_ptr::test_access_ok_accepts_user_range);
    RUN_TEST(test_user_ptr::test_access_ok_rejects_kernel_addr);
    RUN_TEST(test_user_ptr::test_access_ok_rejects_null);
    RUN_TEST(test_user_ptr::test_access_ok_rejects_wraparound);
    RUN_TEST(test_user_ptr::test_copy_rejects_kernel_addr);

    TEST_SECTION("F-EXTABLE accessor fault recovery (B4)");
    RUN_TEST(test_extable::test_copy_from_unmapped_returns_false);
    RUN_TEST(test_extable::test_copy_to_unmapped_returns_false);
    TEST_SUMMARY();
}
