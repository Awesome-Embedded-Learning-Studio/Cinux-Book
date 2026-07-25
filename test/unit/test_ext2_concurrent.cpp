/**
 * @file test/unit/test_ext2_concurrent.cpp
 * @brief F6-M6 B2: ext2 host TSAN concurrency test
 *
 * The payoff of the whole M6 host-build effort: TSan can see data races the
 * kernel's lockdep cannot (lockdep only does lock-order; it is blind to two
 * threads touching a field with NO lock). The F-DYN-COV batches 3/4 hunted the
 * inode_cache_ and block_alloc_ SMP races on QEMU with forensics (LOCKDEP held
 * stacks + RaceWatchpoint + wild traces); this test surfaces the same classes
 * of bug in milliseconds on the host.
 *
 * Two stressors, each N threads over the SAME Ext2 + RAMBlockDevice:
 *   - alloc_block / free_block hammer the block bitmap under block_alloc_lock_
 *   - concurrent lookup("/etc/motd") hammer the inode cache under
 *     inode_cache_lock_ (read-only; each ref'd result is unref'd)
 * If a lock is missing, TSan reports the racing access. Image path injected by
 * CMake (EXT2_TEST_IMG_PATH). Build: -DCINUX_HOST_TSAN=ON.
 *
 * Compile condition: -DCINUX_HOST_TEST
 */

#define TEST_FRAMEWORK_IMPL
#include "test_framework.h"

#ifdef CINUX_HOST_TEST

#    include <atomic>
#    include <cinux/expected.hpp>
#    include <cstdint>
#    include <fstream>
#    include <iterator>
#    include <thread>
#    include <vector>

#    include "kernel/drivers/ram_block_device.hpp"
#    include "kernel/fs/file.hpp"  // inode_unref
#    include "libs/ext2/ext2.hpp"

#    ifndef EXT2_TEST_IMG_PATH
#        error "EXT2_TEST_IMG_PATH must be defined by CMake (source path to the image)"
#    endif

using cinux::drivers::RAMBlockDevice;
using cinux::fs::Ext2;
using cinux::fs::inode_unref;

// N threads each perform 100 alloc/free cycles on the SAME Ext2 instance. All
// bitmap / superblock / BGDT updates run under block_alloc_lock_; a missing
// guard shows up as a TSan data-race report on the bitmap bytes or free-count
// fields.
TEST("ext2_concurrent: parallel alloc_block / free_block (TSAN)") {
    std::ifstream img(EXT2_TEST_IMG_PATH, std::ios::binary);
    ASSERT_TRUE(img.good());
    std::vector<char> bytes((std::istreambuf_iterator<char>(img)),
                            std::istreambuf_iterator<char>());
    ASSERT_TRUE(bytes.size() > 0 && bytes.size() % 512 == 0);
    const uint64_t nblocks = bytes.size() / 512;

    auto dev_r = RAMBlockDevice::create(nblocks);
    ASSERT_OK(dev_r);
    RAMBlockDevice dev = std::move(*dev_r);
    ASSERT_OK(dev.write_blocks(0, nblocks, bytes.data()));

    Ext2 ext2(&dev);
    ASSERT_OK(ext2.mount());

    constexpr int kThreads = 4;
    constexpr int kIters   = 100;
    std::thread   threads[kThreads];
    for (int t = 0; t < kThreads; ++t) {
        threads[t] = std::thread([&ext2]() {
            for (int i = 0; i < kIters; ++i) {
                uint32_t b = ext2.alloc_block();
                if (b != 0) {
                    ext2.free_block(b);
                }
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }
}

// Regression for the shared block_buf_ race found by the original M6 TSan run:
// lookup_in_dir() now owns one KmBuf per call and reuses it across directory
// blocks, so concurrent path resolution never shares scratch storage.
TEST("ext2_concurrent: parallel lookup (TSAN)") {
    std::ifstream img(EXT2_TEST_IMG_PATH, std::ios::binary);
    ASSERT_TRUE(img.good());
    std::vector<char> bytes((std::istreambuf_iterator<char>(img)),
                            std::istreambuf_iterator<char>());
    ASSERT_TRUE(bytes.size() > 0 && bytes.size() % 512 == 0);
    const uint64_t nblocks = bytes.size() / 512;

    auto dev_r = RAMBlockDevice::create(nblocks);
    ASSERT_OK(dev_r);
    RAMBlockDevice dev = std::move(*dev_r);
    ASSERT_OK(dev.write_blocks(0, nblocks, bytes.data()));

    Ext2 ext2(&dev);
    ASSERT_OK(ext2.mount());

    constexpr int    kThreads = 4;
    constexpr int    kIters   = 200;
    std::atomic<int> failures{0};
    std::thread      threads[kThreads];
    for (int t = 0; t < kThreads; ++t) {
        threads[t] = std::thread([&ext2, &failures]() {
            for (int i = 0; i < kIters; ++i) {
                auto result = ext2.lookup("/etc/motd");
                if (!result.ok() || result.value() == nullptr) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                inode_unref(result.value());
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }

    ASSERT_EQ(failures.load(std::memory_order_relaxed), 0);
}

int main() {
    RUN_ALL_TESTS();
    return _tests_failed > 0 ? 1 : 0;
}

#endif  // CINUX_HOST_TEST
