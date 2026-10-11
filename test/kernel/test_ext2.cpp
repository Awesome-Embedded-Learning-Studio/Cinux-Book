#include <stdint.h>

#include "cinux/memory.hpp"
#include "framework_kernel.hpp"
#include "kernel/driver/ahci/ahci.hpp"
#include "kernel/driver/block/block_cache.hpp"
#include "kernel/driver/block/block_queue.hpp"
#include "kernel/fs/ext2/ext2.hpp"
#include "kernel/fs/fs_config.hpp"
#include "kernel/fs/vfs.hpp"
#include "test_assert.hpp"
#include "test_case.hpp"  // NOLINT(misc-include-cleaner) consumed by the TEST() macro body

namespace {

constexpr unsigned long kHelloBytes = 23;

bool same_text(const char* left, const char* right) {
    while (*left != '\0' && *right != '\0' && *left == *right) {
        ++left;
        ++right;
    }
    return *left == *right;
}

bool matches_hello(const uint8_t* bytes, unsigned long count) {
    static constexpr char kHello[] = "hello from the platter\n";
    if (count != sizeof(kHello) - 1) {
        return false;
    }
    return cinux::base::EqualBytes(bytes, kHello, count);
}

void fill_pattern(uint8_t* bytes, unsigned long count) {
    for (unsigned long index = 0; index < count; ++index) {
        bytes[index] = static_cast<uint8_t>(((index * 7U) + 3U) % 251U);
    }
}

}  // namespace

TEST("ext2: the baked volume mounts") {
    cinux::driver::BlockQueue queue(cinux::driver::Ahci::self());
    cinux::driver::BlockCache cache(queue, 2, 8);
    cinux::fs::ext2::Ext2     volume(cache);
    const auto                kMount = volume.mount();
    ASSERT_TRUE(kMount.ok());
}

TEST("ext2: hello.txt is found with its baked size") {
    cinux::driver::BlockQueue queue(cinux::driver::Ahci::self());
    cinux::driver::BlockCache cache(queue, 2, 8);
    cinux::fs::ext2::Ext2     volume(cache);
    ASSERT_TRUE(volume.mount().ok());

    const auto kNode = volume.lookup("hello.txt");
    ASSERT_TRUE(kNode.ok());
    ASSERT_TRUE(kNode.value().size == kHelloBytes);
}

TEST("ext2: hello.txt reads back its baked bytes") {
    cinux::driver::BlockQueue queue(cinux::driver::Ahci::self());
    cinux::driver::BlockCache cache(queue, 2, 8);
    cinux::fs::ext2::Ext2     volume(cache);
    ASSERT_TRUE(volume.mount().ok());

    const auto kNode = volume.lookup("hello.txt");
    ASSERT_TRUE(kNode.ok());
    uint8_t    bytes[kHelloBytes] = {};
    const auto kRead              = volume.read(kNode.value(), 0, bytes, sizeof(bytes));
    ASSERT_TRUE(kRead.ok());
    ASSERT_TRUE(kRead.value() == kHelloBytes);
    ASSERT_TRUE(matches_hello(bytes, sizeof(bytes)));
}

TEST("ext2: stat reports the baked file's own stamp") {
    cinux::driver::BlockQueue queue(cinux::driver::Ahci::self());
    cinux::driver::BlockCache cache(queue, 2, 8);
    cinux::fs::ext2::Ext2     volume(cache);
    ASSERT_TRUE(volume.mount().ok());

    const auto kNode = volume.lookup("hello.txt");
    ASSERT_TRUE(kNode.ok());
    cinux::fs::FsStat probed{};
    ASSERT_TRUE(volume.stat(kNode.value(), probed).ok());
    ASSERT_TRUE(probed.size == kHelloBytes);
    ASSERT_TRUE(probed.mode == 0100644);
    ASSERT_TRUE(probed.links == 1);
    ASSERT_TRUE(probed.mtime > 0);
}

namespace {

bool collect_root_names(cinux::fs::ext2::Ext2& volume, char (&kept)[32][cinux::fs::kFsNameMax],
                        unsigned long*         count_out) {
    const auto kRoot = volume.lookup("");
    if (!kRoot.ok()) {
        return false;
    }
    unsigned long count = 0;
    for (unsigned long index = 0; index < 32; ++index) {
        cinux::fs::FsDirent entry{};
        const auto          kOne = volume.read_dir(kRoot.value(), index, entry);
        if (!kOne.ok()) {
            return false;
        }
        if (!kOne.value()) {
            break;
        }
        cinux::base::CopyBytes(kept[index], entry.name, cinux::fs::kFsNameMax);
        ++count;
    }
    *count_out = count;
    return true;
}

}  // namespace

TEST("ext2: the root directory lists the baked entries") {
    cinux::driver::BlockQueue queue(cinux::driver::Ahci::self());
    cinux::driver::BlockCache cache(queue, 2, 8);
    cinux::fs::ext2::Ext2     volume(cache);
    ASSERT_TRUE(volume.mount().ok());

    static char   kept[32][cinux::fs::kFsNameMax];
    unsigned long count = 0;
    ASSERT_TRUE(collect_root_names(volume, kept, &count));
    ASSERT_TRUE(count >= 5);
    ASSERT_TRUE(same_text(kept[0], "."));
    ASSERT_TRUE(same_text(kept[1], ".."));

    unsigned long baked = 0;
    for (unsigned long i = 0; i < count; ++i) {
        if (same_text(kept[i], "lost+found") || same_text(kept[i], "etc") ||
            same_text(kept[i], "hello.txt")) {
            ++baked;
        }
    }
    ASSERT_TRUE(baked == 3);
}

TEST("ext2: a fresh file takes bytes and grows") {
    cinux::driver::BlockQueue queue(cinux::driver::Ahci::self());
    cinux::driver::BlockCache cache(queue, 2, 8);
    cinux::fs::ext2::Ext2     volume(cache);
    ASSERT_TRUE(volume.mount().ok());

    const auto kMade = volume.make_file("ktest-grown.txt");
    ASSERT_TRUE(kMade.ok());

    uint8_t payload[1000] = {};
    fill_pattern(payload, sizeof(payload));
    const auto kStored = volume.write(kMade.value(), 0, payload, sizeof(payload));
    ASSERT_TRUE(kStored.ok());
    ASSERT_TRUE(kStored.value() == sizeof(payload));

    const auto kNode = volume.lookup("ktest-grown.txt");
    ASSERT_TRUE(kNode.ok());
    ASSERT_TRUE(kNode.value().size == sizeof(payload));
    ASSERT_TRUE(volume.unlink("ktest-grown.txt").ok());
}

TEST("ext2: a written file reads back and unlinks") {
    cinux::driver::BlockQueue queue(cinux::driver::Ahci::self());
    cinux::driver::BlockCache cache(queue, 2, 8);
    cinux::fs::ext2::Ext2     volume(cache);
    ASSERT_TRUE(volume.mount().ok());

    const auto kMade = volume.make_file("ktest-written.txt");
    ASSERT_TRUE(kMade.ok());
    uint8_t payload[1000] = {};
    fill_pattern(payload, sizeof(payload));
    ASSERT_TRUE(volume.write(kMade.value(), 0, payload, sizeof(payload)).ok());

    uint8_t    back[sizeof(payload)] = {};
    const auto kRead                 = volume.read(kMade.value(), 0, back, sizeof(back));
    ASSERT_TRUE(kRead.ok());
    ASSERT_TRUE(cinux::base::EqualBytes(payload, back, sizeof(payload)));

    const auto kGone = volume.unlink("ktest-written.txt");
    ASSERT_TRUE(kGone.ok());
    ASSERT_TRUE(!volume.lookup("ktest-written.txt").ok());
}

TEST("ext2: a made directory lists and empties") {
    cinux::driver::BlockQueue queue(cinux::driver::Ahci::self());
    cinux::driver::BlockCache cache(queue, 2, 8);
    cinux::fs::ext2::Ext2     volume(cache);
    ASSERT_TRUE(volume.mount().ok());

    const auto kMade = volume.make_dir("ktest-dir");
    ASSERT_TRUE(kMade.ok());

    cinux::fs::FsDirent first{};
    const auto          kFirst = volume.read_dir(volume.lookup("ktest-dir").value(), 0, first);
    ASSERT_TRUE(kFirst.ok() && kFirst.value());
    ASSERT_TRUE(same_text(first.name, "."));

    const auto kRemoved = volume.unlink("ktest-dir");
    ASSERT_TRUE(kRemoved.ok());
    ASSERT_TRUE(!volume.lookup("ktest-dir").ok());
}
