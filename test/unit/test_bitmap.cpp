#include "../framework/framework.hpp"
#include "cinux/bit_ops/bitmap.hpp"
#include "cinux/literal_types.hpp"

using cinux::base::bit::Bitmap;
using cinux::base::bit::kInvalidIndex;
using cinux::base::bit::WordMask;

using cinux::base::operator""_GiB;
using cinux::base::operator""_KiB;
using cinux::base::operator""_MiB;
using cinux::base::operator""_TiB;

TEST("bitmap: byte-unit literals are pinned") {
    ASSERT_TRUE(2_KiB == 2048);
    ASSERT_TRUE(3_MiB == 0x300000);
    ASSERT_TRUE(1_GiB == 0x40000000);
    ASSERT_TRUE(1_TiB == 0x10000000000ULL);
}

TEST("bitmap: words_for covers the tail") {
    ASSERT_TRUE(Bitmap::words_for(0) == 0);
    ASSERT_TRUE(Bitmap::words_for(1) == 1);
    ASSERT_TRUE(Bitmap::words_for(64) == 1);
    ASSERT_TRUE(Bitmap::words_for(65) == 2);
    ASSERT_TRUE(Bitmap::words_for(70) == 2);
    ASSERT_TRUE(Bitmap::words_for(128) == 2);
}

TEST("bitmap: set clear test and counts roundtrip") {
    WordMask storage[4] = {};
    Bitmap   map{};
    map.init(storage, 200);
    ASSERT_TRUE(map.set_count() == 0);
    ASSERT_TRUE(map.clear_count() == 200);
    ASSERT_TRUE(!map.test(0));
    map.set(0);
    map.set(199);
    map.set(64);
    ASSERT_TRUE(map.test(0));
    ASSERT_TRUE(map.test(199));
    ASSERT_TRUE(map.test(64));
    ASSERT_TRUE(map.set_count() == 3);
}

TEST("bitmap: repeated set and clear stay idempotent") {
    WordMask storage[4] = {};
    Bitmap   map{};
    map.init(storage, 200);
    map.set(7);
    map.set(7);
    ASSERT_TRUE(map.set_count() == 1);
    map.clear(7);
    map.clear(7);
    ASSERT_TRUE(map.set_count() == 0);
    ASSERT_TRUE(!map.test(7));
}

TEST("bitmap: set_all keeps the tail word honest") {
    WordMask storage[2] = {};
    Bitmap   map{};
    map.init(storage, 70);
    map.set_all();
    ASSERT_TRUE(map.set_count() == 70);
    ASSERT_TRUE(map.test(69));
    ASSERT_TRUE(map.find_clear_run(1) == kInvalidIndex);
    map.clear(65);
    ASSERT_TRUE(map.find_clear_run(1) == 65);
    map.clear_all();
    ASSERT_TRUE(map.set_count() == 0);
    ASSERT_TRUE(map.find_clear_run(70) == 0);
}

TEST("bitmap: set_range spans exactly its count") {
    WordMask storage[2] = {};
    Bitmap   map{};
    map.init(storage, 100);
    map.set_range(10, 5);
    ASSERT_TRUE(map.set_count() == 5);
    ASSERT_TRUE(map.test(10));
    ASSERT_TRUE(map.test(14));
    ASSERT_TRUE(!map.test(9));
    ASSERT_TRUE(!map.test(15));
}

TEST("bitmap: clear_range carves from a range") {
    WordMask storage[2] = {};
    Bitmap   map{};
    map.init(storage, 100);
    map.set_range(10, 5);
    map.clear_range(12, 2);
    ASSERT_TRUE(map.set_count() == 3);
    ASSERT_TRUE(!map.test(12));
    ASSERT_TRUE(map.test(11));
    ASSERT_TRUE(map.test(14));
}

TEST("bitmap: find_clear_run lands on holes") {
    WordMask storage[1] = {};
    Bitmap   map{};
    map.init(storage, 8);
    ASSERT_TRUE(map.find_clear_run(1) == 0);
    map.set(2);
    map.set(5);
    ASSERT_TRUE(map.find_clear_run(1) == 0);
    ASSERT_TRUE(map.find_clear_run(2) == 0);
    ASSERT_TRUE(map.find_clear_run(3) == kInvalidIndex);
    map.set(0);
    map.set(1);
    ASSERT_TRUE(map.find_clear_run(2) == 3);
}

TEST("bitmap: runs cross word boundaries") {
    WordMask storage[3] = {};
    Bitmap   map{};
    map.init(storage, 130);
    map.set_all();
    map.clear(63);
    map.clear(64);
    map.clear(65);
    ASSERT_TRUE(map.find_clear_run(3) == 63);
    ASSERT_TRUE(map.find_clear_run(4) == kInvalidIndex);
    map.clear(126);
    map.clear(127);
    ASSERT_TRUE(map.find_clear_run(2) == 63);
    map.set(63);
    ASSERT_TRUE(map.find_clear_run(2) == 64);
}

TEST("bitmap: whole-capacity run and rejects") {
    WordMask storage[1] = {};
    Bitmap   map{};
    map.init(storage, 64);
    ASSERT_TRUE(map.find_clear_run(64) == 0);
    ASSERT_TRUE(map.find_clear_run(65) == kInvalidIndex);
    ASSERT_TRUE(map.find_clear_run(0) == kInvalidIndex);
}

int main() {
    return cinux::test::RunAll();
}
