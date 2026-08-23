/**
 * @file test/unit/test_static_hash_map.cpp
 * @brief Tests for cinux::lib::StaticHashMap
 */

#include <catch2/catch_test_macros.hpp>
#include <cinux/static_hash_map.hpp>

using namespace cinux::lib;

TEST_CASE("StaticHashMap: insert and find", "[hash_map]") {
    StaticHashMap<int, int, 16> map;
    REQUIRE(map.insert(1, 10));
    REQUIRE(map.insert(2, 20));

    auto v = map.find(1);
    REQUIRE(v.has_value());
    REQUIRE(v.value() == 10);

    auto v2 = map.find(2);
    REQUIRE(v2.has_value());
    REQUIRE(v2.value() == 20);
}

TEST_CASE("StaticHashMap: key already exists returns false", "[hash_map]") {
    StaticHashMap<int, int, 16> map;
    REQUIRE(map.insert(1, 10));
    REQUIRE_FALSE(map.insert(1, 99));
    // Original value unchanged
    REQUIRE(map.find(1).value() == 10);
}

TEST_CASE("StaticHashMap: insert_or_assign overwrites", "[hash_map]") {
    StaticHashMap<int, int, 16> map;
    map.insert(1, 10);
    REQUIRE(map.insert_or_assign(1, 99));
    REQUIRE(map.find(1).value() == 99);
}

TEST_CASE("StaticHashMap: remove and tombstone reuse", "[hash_map]") {
    StaticHashMap<int, int, 16> map;
    map.insert(1, 10);
    REQUIRE(map.remove(1));
    REQUIRE_FALSE(map.find(1).has_value());
    REQUIRE(map.size() == 0);

    // Re-insert same key should succeed (tombstone reuse)
    REQUIRE(map.insert(1, 20));
    REQUIRE(map.find(1).value() == 20);
}

TEST_CASE("StaticHashMap: contains", "[hash_map]") {
    StaticHashMap<int, int, 16> map;
    map.insert(5, 50);
    REQUIRE(map.contains(5));
    REQUIRE_FALSE(map.contains(6));
}

TEST_CASE("StaticHashMap: full", "[hash_map]") {
    StaticHashMap<int, int, 4> map;
    REQUIRE(map.insert(1, 10));
    REQUIRE(map.insert(2, 20));
    REQUIRE(map.insert(3, 30));
    REQUIRE(map.insert(4, 40));
    REQUIRE(map.full());
    REQUIRE_FALSE(map.insert(5, 50));
}

TEST_CASE("StaticHashMap: StringView key", "[hash_map]") {
    StaticHashMap<StringView, int, 16> map;
    map.insert("hello", 1);
    map.insert("world", 2);

    REQUIRE(map.find("hello").value() == 1);
    REQUIRE(map.find("world").value() == 2);
    REQUIRE_FALSE(map.find("foo").has_value());
}

TEST_CASE("StaticHashMap: operator[] default insert", "[hash_map]") {
    StaticHashMap<int, int, 8> map;
    map[42] = 100;
    REQUIRE(map.find(42).value() == 100);
    map[42] = 200;  // overwrite
    REQUIRE(map[42] == 200);
}

TEST_CASE("StaticHashMap: clear", "[hash_map]") {
    StaticHashMap<int, int, 8> map;
    map.insert(1, 10);
    map.insert(2, 20);
    map.clear();
    REQUIRE(map.empty());
    REQUIRE_FALSE(map.find(1).has_value());
}
