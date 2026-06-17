/**
 * @file kernel/test/test_page_cache.cpp
 * @brief QEMU in-kernel tests for the file-backed PageCache (F2-M4 batch 1)
 *
 * Each case uses a fresh LOCAL PageCache (no global state) and a fake Inode
 * whose InodeOps::read serves bytes from an in-memory buffer -- the same shape
 * as RamdiskFileOps.  Validates lookup hit/miss, get_page fill + caching, ref
 * counting, EOF zero-padding, and hit/miss stats.  No scheduler or address
 * space is involved: we exercise the cache directly.
 */

#include <stddef.h>
#include <stdint.h>

#include "big_kernel_test.h"
#include "kernel/arch/x86_64/memory_layout.hpp"
#include "kernel/arch/x86_64/paging_config.hpp"
#include "kernel/fs/inode.hpp"
#include "kernel/lib/string.hpp"
#include "kernel/mm/page_cache.hpp"

using cinux::arch::KERNEL_VMA;
using cinux::arch::PAGE_SIZE;
using cinux::fs::Inode;
using cinux::fs::InodeOps;
using cinux::fs::InodeType;
using cinux::mm::CachedPage;
using cinux::mm::PageCache;

namespace {

/// In-memory file backing exposed through InodeOps::read (RamdiskFileOps shape).
struct FakeFile {
    static constexpr uint64_t kCapacity = 2 * PAGE_SIZE;  // two full pages
    uint8_t                   data[kCapacity];
    uint64_t                  size;
};

/// Deterministic fill so tests can verify byte-exact content per offset.
void fill_fake(FakeFile& f, uint64_t size) {
    f.size = (size < FakeFile::kCapacity) ? size : FakeFile::kCapacity;
    for (uint64_t i = 0; i < f.size; i++) {
        f.data[i] = static_cast<uint8_t>((i * 7u + 3u) & 0xFFu);
    }
}

/// Fake InodeOps: serve bytes from the FakeFile pinned at inode->fs_private.
class FakeFileOps : public InodeOps {
public:
    /// Number of times read() was invoked (a stand-in for "disk reads"), so the
    /// read_bytes tests can prove the cache avoids re-reading on a hit.
    uint64_t read_calls{0};

    cinux::lib::ErrorOr<int64_t> read(const Inode* inode, uint64_t offset, void* buf,
                                      uint64_t count) override {
        if (inode == nullptr || buf == nullptr || inode->fs_private == nullptr) {
            return cinux::lib::Error::InvalidArgument;
        }
        const auto* f = static_cast<const FakeFile*>(inode->fs_private);
        if (offset >= f->size) {
            return 0;  // EOF
        }
        uint64_t avail = f->size - offset;
        uint64_t nread = (count < avail) ? count : avail;
        memcpy(buf, f->data + offset, nread);
        read_calls++;
        return static_cast<int64_t>(nread);
    }
};

FakeFileOps g_fake_ops;

/// Scratch buffer for read_bytes tests (kept off the kernel stack).
uint8_t g_readbuf[2 * PAGE_SIZE];

Inode make_fake_inode(FakeFile& f) {
    Inode ino{};
    ino.ino        = 1;
    ino.size       = f.size;
    ino.type       = InodeType::Regular;
    ino.ops        = &g_fake_ops;
    ino.fs_private = &f;
    return ino;
}

/// Read byte @p idx from a cached page's direct-mapped kernel virtual address.
uint8_t byte_at(const CachedPage* p, uint64_t idx) {
    return reinterpret_cast<uint8_t*>(p->virt)[idx];
}

}  // namespace

// ============================================================
// Test 1: lookup misses before any get_page
// ============================================================

namespace test_pc_miss {

void test_lookup_miss_initially() {
    PageCache cache;
    cache.init(64);
    FakeFile f;
    fill_fake(f, 2 * PAGE_SIZE);
    Inode ino = make_fake_inode(f);

    TEST_ASSERT_NULL(cache.lookup(&ino, 0));
    TEST_ASSERT_NULL(cache.lookup(&ino, PAGE_SIZE));
    TEST_ASSERT_EQ(cache.cached_pages(), static_cast<size_t>(0));
}

}  // namespace test_pc_miss

// ============================================================
// Test 2: get_page fills + caches page 0 with correct content
// ============================================================

namespace test_pc_fill {

void test_get_page_fills_and_caches() {
    PageCache cache;
    cache.init(64);
    FakeFile f;
    fill_fake(f, 2 * PAGE_SIZE);
    Inode ino = make_fake_inode(f);

    auto r = cache.get_page(&ino, 0);
    TEST_ASSERT_TRUE(r.ok());
    CachedPage* p = r.value();
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_TRUE(p->valid);
    TEST_ASSERT_EQ(p->offset, static_cast<uint64_t>(0));
    TEST_ASSERT_EQ(p->phys, p->virt - KERNEL_VMA);  // direct-map invariant
    // Byte-exact content sampling across a full page.
    for (uint64_t i = 0; i < PAGE_SIZE; i += 256) {
        TEST_ASSERT_EQ(byte_at(p, i), f.data[i]);
    }
    TEST_ASSERT_EQ(cache.cached_pages(), static_cast<size_t>(1));
    TEST_ASSERT_EQ(cache.miss_count(), static_cast<size_t>(1));
    TEST_ASSERT_EQ(cache.hit_count(), static_cast<size_t>(0));
    TEST_ASSERT_EQ(p->ref_count, 1u);
}

}  // namespace test_pc_fill

// ============================================================
// Test 3: second get_page of the same key hits (same phys, ref++)
// ============================================================

namespace test_pc_hit {

void test_second_get_hits() {
    PageCache cache;
    cache.init(64);
    FakeFile f;
    fill_fake(f, 2 * PAGE_SIZE);
    Inode ino = make_fake_inode(f);

    auto r1 = cache.get_page(&ino, 0);
    auto r2 = cache.get_page(&ino, 0);
    TEST_ASSERT_TRUE(r1.ok());
    TEST_ASSERT_TRUE(r2.ok());
    TEST_ASSERT_TRUE(r1.value() == r2.value());  // same CachedPage
    TEST_ASSERT_TRUE(r1.value()->phys == r2.value()->phys);
    TEST_ASSERT_EQ(cache.cached_pages(), static_cast<size_t>(1));  // no new page
    TEST_ASSERT_EQ(cache.hit_count(), static_cast<size_t>(1));
    TEST_ASSERT_EQ(cache.miss_count(), static_cast<size_t>(1));
    TEST_ASSERT_EQ(r2.value()->ref_count, 2u);

    TEST_ASSERT_NOT_NULL(cache.lookup(&ino, 0));
}

}  // namespace test_pc_hit

// ============================================================
// Test 4: distinct offsets cache as separate pages
// ============================================================

namespace test_pc_distinct {

void test_distinct_offsets() {
    PageCache cache;
    cache.init(64);
    FakeFile f;
    fill_fake(f, 2 * PAGE_SIZE);
    Inode ino = make_fake_inode(f);

    auto r0 = cache.get_page(&ino, 0);
    auto r1 = cache.get_page(&ino, PAGE_SIZE);
    TEST_ASSERT_TRUE(r0.ok() && r1.ok());
    TEST_ASSERT_TRUE(r0.value() != r1.value());
    TEST_ASSERT_TRUE(r0.value()->phys != r1.value()->phys);
    TEST_ASSERT_EQ(cache.cached_pages(), static_cast<size_t>(2));
    // Page 1 content correctness.
    TEST_ASSERT_EQ(byte_at(r1.value(), 0), f.data[PAGE_SIZE]);
    TEST_ASSERT_EQ(byte_at(r1.value(), PAGE_SIZE - 1), f.data[2 * PAGE_SIZE - 1]);
}

}  // namespace test_pc_distinct

// ============================================================
// Test 5: EOF tail is zero-padded
// ============================================================

namespace test_pc_eof {

void test_eof_zero_pad() {
    PageCache cache;
    cache.init(64);
    FakeFile f;
    fill_fake(f, PAGE_SIZE + 100);  // page 1 holds 100 real bytes
    Inode ino = make_fake_inode(f);

    auto r = cache.get_page(&ino, PAGE_SIZE);
    TEST_ASSERT_TRUE(r.ok());
    CachedPage* p = r.value();
    TEST_ASSERT_EQ(byte_at(p, 0), f.data[PAGE_SIZE]);                    // first real byte
    TEST_ASSERT_EQ(byte_at(p, 99), f.data[PAGE_SIZE + 99]);              // last real byte
    TEST_ASSERT_EQ(byte_at(p, 100), static_cast<uint8_t>(0));            // pad begins
    TEST_ASSERT_EQ(byte_at(p, PAGE_SIZE - 1), static_cast<uint8_t>(0));  // pad to end
}

}  // namespace test_pc_eof

// ============================================================
// Test 6: release decrements ref count (floors at 0, no eviction)
// ============================================================

namespace test_pc_release {

void test_release_decrements() {
    PageCache cache;
    cache.init(64);
    FakeFile f;
    fill_fake(f, 2 * PAGE_SIZE);
    Inode ino = make_fake_inode(f);

    cache.get_page(&ino, 0);
    auto r2 = cache.get_page(&ino, 0);  // ref -> 2
    TEST_ASSERT_EQ(r2.value()->ref_count, 2u);

    cache.release(r2.value());
    TEST_ASSERT_EQ(r2.value()->ref_count, 1u);
    cache.release(r2.value());
    TEST_ASSERT_EQ(r2.value()->ref_count, 0u);
    cache.release(r2.value());  // floor
    TEST_ASSERT_EQ(r2.value()->ref_count, 0u);
    // Page stays cached (no eviction in MVP).
    TEST_ASSERT_EQ(cache.cached_pages(), static_cast<size_t>(1));
    TEST_ASSERT_NOT_NULL(cache.lookup(&ino, 0));
}

}  // namespace test_pc_release

// ============================================================
// Test 7 (F2-M6): read_bytes serves correct bytes and honours EOF
// ============================================================

namespace test_pc_read_bytes {

void test_read_bytes_basic_and_eof() {
    PageCache cache;
    cache.init(64);
    FakeFile f;
    fill_fake(f, 100);  // 100 bytes, well under one page
    Inode ino = make_fake_inode(f);

    g_fake_ops.read_calls = 0;
    auto r                = cache.read_bytes(&ino, 0, g_readbuf, 100);
    TEST_ASSERT_TRUE(r.ok());
    TEST_ASSERT_EQ(static_cast<uint64_t>(r.value()), 100u);
    TEST_ASSERT_EQ(memcmp(g_readbuf, f.data, 100), 0);
    // One miss fills the whole underlying page (PAGE_SIZE read at offset 0).
    TEST_ASSERT_EQ(g_fake_ops.read_calls, 1u);

    // At/past EOF returns 0 with no read.
    g_fake_ops.read_calls = 0;
    auto r2               = cache.read_bytes(&ino, 100, g_readbuf, 10);
    TEST_ASSERT_TRUE(r2.ok());
    TEST_ASSERT_EQ(static_cast<uint64_t>(r2.value()), 0u);
    TEST_ASSERT_EQ(g_fake_ops.read_calls, 0u);
}

}  // namespace test_pc_read_bytes

// ============================================================
// Test 8 (F2-M6): second read_bytes is served from cache (no re-read)
// ============================================================

namespace test_pc_read_bytes_cache {

void test_second_read_hits_cache() {
    PageCache cache;
    cache.init(64);
    FakeFile f;
    fill_fake(f, 2 * PAGE_SIZE);
    Inode ino = make_fake_inode(f);

    g_fake_ops.read_calls = 0;
    size_t hits_before    = cache.hit_count();

    auto r1 = cache.read_bytes(&ino, 0, g_readbuf, 2 * PAGE_SIZE);
    TEST_ASSERT_TRUE(r1.ok());
    TEST_ASSERT_EQ(static_cast<uint64_t>(r1.value()), 2 * PAGE_SIZE);
    TEST_ASSERT_EQ(memcmp(g_readbuf, f.data, 2 * PAGE_SIZE), 0);
    // Two underlying pages filled (page 0 + page 1).
    uint64_t reads_after_first = g_fake_ops.read_calls;
    TEST_ASSERT_EQ(reads_after_first, 2u);

    // Identical second read: every page is now cached, so no new disk read and
    // the hit counter climbs -- the whole point of routing read() through cache.
    auto r2 = cache.read_bytes(&ino, 0, g_readbuf, 2 * PAGE_SIZE);
    TEST_ASSERT_TRUE(r2.ok());
    TEST_ASSERT_EQ(static_cast<uint64_t>(r2.value()), 2 * PAGE_SIZE);
    TEST_ASSERT_EQ(g_fake_ops.read_calls, reads_after_first);  // no extra reads
    TEST_ASSERT_GT(cache.hit_count(), hits_before);
}

}  // namespace test_pc_read_bytes_cache

// ============================================================
// Test 9 (F2-M6): read_bytes clips to EOF mid-page
// ============================================================

namespace test_pc_read_bytes_eof {

void test_read_bytes_partial_at_eof() {
    PageCache cache;
    cache.init(64);
    FakeFile f;
    fill_fake(f, PAGE_SIZE + 50);  // page 1 holds only 50 real bytes
    Inode ino = make_fake_inode(f);

    auto r = cache.read_bytes(&ino, PAGE_SIZE, g_readbuf, PAGE_SIZE);
    TEST_ASSERT_TRUE(r.ok());
    // Asked for a full page but only 50 bytes remain before EOF.
    TEST_ASSERT_EQ(static_cast<uint64_t>(r.value()), 50u);
    TEST_ASSERT_EQ(memcmp(g_readbuf, f.data + PAGE_SIZE, 50), 0);
}

}  // namespace test_pc_read_bytes_eof

// ============================================================
// Entry point
// ============================================================

extern "C" void run_page_cache_tests() {
    TEST_SECTION("PageCache Tests (F2-M4-1)");

    RUN_TEST(test_pc_miss::test_lookup_miss_initially);
    RUN_TEST(test_pc_fill::test_get_page_fills_and_caches);
    RUN_TEST(test_pc_hit::test_second_get_hits);
    RUN_TEST(test_pc_distinct::test_distinct_offsets);
    RUN_TEST(test_pc_eof::test_eof_zero_pad);
    RUN_TEST(test_pc_release::test_release_decrements);

    RUN_TEST(test_pc_read_bytes::test_read_bytes_basic_and_eof);
    RUN_TEST(test_pc_read_bytes_cache::test_second_read_hits_cache);
    RUN_TEST(test_pc_read_bytes_eof::test_read_bytes_partial_at_eof);

    TEST_SUMMARY();
}
