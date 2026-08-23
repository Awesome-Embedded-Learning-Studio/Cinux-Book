/**
 * @file test/unit/test_algorithm.cpp
 * @brief Tests for cinux::lib::Algorithm utilities
 */

#include <catch2/catch_test_macros.hpp>
#include <cinux/algorithm.hpp>

using namespace cinux::lib;

TEST_CASE("algorithm: min/max/clamp", "[algorithm]") {
    REQUIRE(min(3, 5) == 3);
    REQUIRE(max(3, 5) == 5);
    REQUIRE(clamp(7, 1, 10) == 7);
    REQUIRE(clamp(-1, 0, 10) == 0);
    REQUIRE(clamp(99, 0, 10) == 10);
}

TEST_CASE("algorithm: swap", "[algorithm]") {
    int a = 10, b = 20;
    swap(a, b);
    REQUIRE(a == 20);
    REQUIRE(b == 10);
}

TEST_CASE("algorithm: find", "[algorithm]") {
    int  arr[] = {10, 20, 30, 40};
    int* end   = arr + 4;

    REQUIRE(find(arr, end, 30) == &arr[2]);
    REQUIRE(find(arr, end, 99) == end);
}

TEST_CASE("algorithm: find_if", "[algorithm]") {
    int  arr[] = {1, 4, 7, 2};
    int* end   = arr + 4;

    auto* p = find_if(arr, end, [](int v) { return v > 5; });
    REQUIRE(p == &arr[2]);
    REQUIRE(*p == 7);
}

TEST_CASE("algorithm: fill and copy", "[algorithm]") {
    int src[]  = {1, 2, 3};
    int dst[3] = {};

    copy(src, src + 3, dst);
    REQUIRE(dst[0] == 1);
    REQUIRE(dst[2] == 3);

    fill(dst, dst + 3, 42);
    REQUIRE(dst[0] == 42);
    REQUIRE(dst[2] == 42);
}

TEST_CASE("algorithm: insertion_sort", "[algorithm]") {
    int arr[] = {5, 2, 8, 1, 9, 3};
    insertion_sort(arr, arr + 6);

    REQUIRE(arr[0] == 1);
    REQUIRE(arr[1] == 2);
    REQUIRE(arr[2] == 3);
    REQUIRE(arr[3] == 5);
    REQUIRE(arr[4] == 8);
    REQUIRE(arr[5] == 9);
}

TEST_CASE("algorithm: insertion_sort with comparator", "[algorithm]") {
    int arr[] = {3, 1, 4, 1, 5};
    insertion_sort(arr, arr + 5, [](int a, int b) { return a > b; });

    REQUIRE(arr[0] == 5);
    REQUIRE(arr[4] == 1);
}

TEST_CASE("algorithm: binary_search", "[algorithm]") {
    int arr[] = {1, 3, 5, 7, 9, 11};

    REQUIRE(binary_search(arr, arr + 6, 5) == &arr[2]);
    REQUIRE(binary_search(arr, arr + 6, 1) == &arr[0]);
    REQUIRE(binary_search(arr, arr + 6, 11) == &arr[5]);
    REQUIRE(binary_search(arr, arr + 6, 6) == nullptr);
    REQUIRE(binary_search(arr, arr + 6, 99) == nullptr);
}
