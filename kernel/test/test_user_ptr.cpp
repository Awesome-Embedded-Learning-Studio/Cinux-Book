/**
 * @file kernel/test/test_user_ptr.cpp
 * @brief QEMU in-kernel tests for UserPtr<T> (F-QA Q4a-2)
 *
 * UserPtr is a zero-overhead marker for user-space pointers (sparse __user
 * semantics). These tests exercise its type-level contract: it holds the
 * pointer unchanged, converts back to raw (drop-in via operator T()), supports
 * member access / dereference for typed pointers, allows nullptr (unlike
 * NotNull), and adds no runtime validation (that stays with validate_user_ptr).
 */

#include <stddef.h>
#include <stdint.h>

#include "big_kernel_test.h"
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

}  // namespace test_user_ptr

extern "C" void run_user_ptr_tests() {
    TEST_SECTION("UserPtr (F-QA Q4a-2)");
    RUN_TEST(test_user_ptr::test_default_construct_is_null);
    RUN_TEST(test_user_ptr::test_holds_constructed_pointer);
    RUN_TEST(test_user_ptr::test_implicit_conversion_drop_in);
    RUN_TEST(test_user_ptr::test_member_access_arrow);
    RUN_TEST(test_user_ptr::test_dereference_star);
    RUN_TEST(test_user_ptr::test_nullptr_allowed_unlike_not_null);
    RUN_TEST(test_user_ptr::test_const_char_pointer);
    TEST_SUMMARY();
}
