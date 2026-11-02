#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <ios>
#include <memory>
#include <print>
#include <string>
#include <utility>
#include <vector>

#include "../framework/framework.hpp"
#include "cinux/byte_order.hpp"
#include "cinux/literal_types.hpp"
#include "cinux/memory.hpp"
#include "cinux/result.hpp"
#include "test_assert.hpp"

using cinux::base::operator""_KiB;
using cinux::base::operator""_MiB;

#include "kernel/driver/block/block_cache.hpp"
#include "kernel/driver/block/block_device_concept.hpp"
#include "kernel/driver/block/block_queue.hpp"
#include "kernel/fs/ext2/ext2.hpp"
#include "kernel/fs/ext2/ext2_layout.hpp"
#include "kernel/fs/vfs.hpp"
#include "test/mock/image_disk.hpp"

using cinux::fs::ext2::BlockSlot;
using cinux::fs::ext2::DecodeBlockSize;
using cinux::fs::ext2::DirEntry;
using cinux::fs::ext2::DirEntryLength;
using cinux::fs::ext2::GroupDescriptor;
using cinux::fs::ext2::GroupOf;
using cinux::fs::ext2::Inode;
using cinux::fs::ext2::IndexOfInode;
using cinux::fs::ext2::IsDirectoryMode;
using cinux::fs::ext2::IsRegularMode;
using cinux::fs::ext2::MapFileBlock;
using cinux::fs::ext2::Superblock;

namespace {

void expect_sector_pattern(const std::uint8_t* bytes, unsigned long count) {
    for (unsigned long index = 0; index < count; ++index) {
        ASSERT_EQ(bytes[index], static_cast<std::uint8_t>(1 + (index / 512)));
    }
}

}  // namespace

TEST("block cache: copying reads reject crossings and survive eviction and failed fills") {
    constexpr unsigned long   kPage = 4096;
    std::vector<std::uint8_t> image(2 * kPage);
    for (unsigned long index = 0; index < image.size(); ++index) {
        image[index] = static_cast<std::uint8_t>(index / 512);
    }
    cinux::test::ImageDisk    disk(std::move(image));
    cinux::driver::BlockQueue queue(disk);
    cinux::driver::BlockCache cache(queue, 1);
    std::uint8_t              first[1024]{};
    std::uint8_t              other[1024]{};
    ASSERT_TRUE(cache.read(cinux::driver::Lba{.value = 1}, first, sizeof(first)));
    ASSERT_TRUE(cache.read(cinux::driver::Lba{.value = 8}, other, sizeof(other)));
    expect_sector_pattern(first, sizeof(first));
    ASSERT_FALSE(cache.read(cinux::driver::Lba{.value = 7}, first, sizeof(first)));
    ASSERT_FALSE(cache.read(cinux::driver::Lba{.value = 16}, first, sizeof(first)));
    expect_sector_pattern(first, sizeof(first));
    ASSERT_TRUE(cache.read(cinux::driver::Lba{.value = 1}, first, sizeof(first)));
}

TEST("ext2: on-disk structure sizes") {
    ASSERT_TRUE(sizeof(Superblock) == 1024);
    ASSERT_TRUE(sizeof(GroupDescriptor) == 32);
    ASSERT_TRUE(sizeof(Inode) == 128);
    ASSERT_TRUE(sizeof(DirEntry) == 8);
}

TEST("block cache: device flush failures propagate and clean pages still retry the barrier") {
    cinux::test::ImageDisk    disk(std::vector<std::uint8_t>(4096));
    cinux::driver::BlockQueue queue(disk);
    cinux::driver::BlockCache cache(queue, 1);
    const std::uint8_t        kByte = 31;
    ASSERT_TRUE(cache.patch(cinux::driver::Lba{}, 0, 1, &kByte));
    disk.fail_flushes(true);
    ASSERT_FALSE(cache.flush());
    ASSERT_EQ(disk.flush_count(), 1UL);
    const auto kWrites = disk.write_count();
    disk.fail_flushes(false);
    ASSERT_TRUE(cache.flush());
    ASSERT_EQ(disk.flush_count(), 2UL);
    ASSERT_EQ(disk.write_count(), kWrites);
}

TEST("ext2: superblock and group offsets against the specification") {
    ASSERT_TRUE(offsetof(Superblock, s_magic) == 56);
    ASSERT_TRUE(offsetof(Superblock, s_inode_size) == 88);
    ASSERT_TRUE(offsetof(Superblock, s_inodes_per_group) == 40);
    ASSERT_TRUE(offsetof(GroupDescriptor, bg_inode_table) == 8);
}

TEST("ext2: inode offsets against the specification") {
    ASSERT_TRUE(offsetof(Inode, i_size) == 4);
    ASSERT_TRUE(offsetof(Inode, i_links_count) == 26);
    ASSERT_TRUE(offsetof(Inode, i_flags) == 32);
    ASSERT_TRUE(offsetof(Inode, i_block) == 40);
    ASSERT_TRUE(offsetof(Inode, i_mtime) == 16);
}

TEST("ext2: block size decoding") {
    ASSERT_TRUE(DecodeBlockSize(0) == 1024);
    ASSERT_TRUE(DecodeBlockSize(1) == 2048);
    ASSERT_TRUE(DecodeBlockSize(2) == 4096);
}

TEST("ext2: inode placement arithmetic") {
    ASSERT_TRUE(GroupOf(2, 128) == 0);
    ASSERT_TRUE(IndexOfInode(2, 128) == 1);
    ASSERT_TRUE(GroupOf(129, 128) == 1);
    ASSERT_TRUE(IndexOfInode(129, 128) == 0);
    ASSERT_TRUE(GroupOf(256, 128) == 1);
    ASSERT_TRUE(IndexOfInode(256, 128) == 127);
}

TEST("ext2: directory entry length alignment") {
    ASSERT_TRUE(DirEntryLength(1) == 12);
    ASSERT_TRUE(DirEntryLength(5) == 16);
    ASSERT_TRUE(DirEntryLength(8) == 16);
    ASSERT_TRUE(DirEntryLength(9) == 20);
}

TEST("ext2: direct blocks map to their own slots") {
    const BlockSlot kFirst = MapFileBlock(0);
    ASSERT_TRUE(kFirst.kind == BlockSlot::Kind::kDirect);
    ASSERT_TRUE(kFirst.slot == 0);

    const BlockSlot kLastDirect = MapFileBlock(11);
    ASSERT_TRUE(kLastDirect.kind == BlockSlot::Kind::kDirect);
    ASSERT_TRUE(kLastDirect.slot == 11);
}

TEST("ext2: the single-indirect layer starts at file block 12") {
    const BlockSlot kFirst = MapFileBlock(12);
    ASSERT_TRUE(kFirst.kind == BlockSlot::Kind::kSingle);
    ASSERT_TRUE(kFirst.slot == 12);
    ASSERT_TRUE(kFirst.outer == 0);

    const BlockSlot kLast = MapFileBlock(267);
    ASSERT_TRUE(kLast.kind == BlockSlot::Kind::kSingle);
    ASSERT_TRUE(kLast.slot == 12);
    ASSERT_TRUE(kLast.outer == 255);
}

TEST("ext2: the double-indirect layer starts at file block 268") {
    const BlockSlot kFirst = MapFileBlock(268);
    ASSERT_TRUE(kFirst.kind == BlockSlot::Kind::kDouble);
    ASSERT_TRUE(kFirst.slot == 13);
    ASSERT_TRUE(kFirst.outer == 0);
    ASSERT_TRUE(kFirst.inner == 0);

    const BlockSlot kSecondTable = MapFileBlock(268 + 256);
    ASSERT_TRUE(kSecondTable.kind == BlockSlot::Kind::kDouble);
    ASSERT_TRUE(kSecondTable.outer == 1);
    ASSERT_TRUE(kSecondTable.inner == 0);
}

TEST("ext2: the double-indirect layer ends at its last inner slot") {
    const BlockSlot kLast = MapFileBlock(268 + (256 * 255) + 255);
    ASSERT_TRUE(kLast.kind == BlockSlot::Kind::kDouble);
    ASSERT_TRUE(kLast.outer == 255);
    ASSERT_TRUE(kLast.inner == 255);
}

TEST("ext2: past double is refused loudly") {
    const BlockSlot kPast = MapFileBlock(268 + (256 * 256));
    ASSERT_TRUE(kPast.kind == BlockSlot::Kind::kUnsupported);

    const BlockSlot kFar = MapFileBlock(1000000);
    ASSERT_TRUE(kFar.kind == BlockSlot::Kind::kUnsupported);
}

TEST("ext2: mode type segment reads") {
    ASSERT_TRUE(IsDirectoryMode(0x41ED));
    ASSERT_TRUE(!IsRegularMode(0x41ED));
    ASSERT_TRUE(IsRegularMode(0x81A4));
    ASSERT_TRUE(!IsDirectoryMode(0x81A4));
    ASSERT_TRUE(!IsDirectoryMode(0xA1FF));
    ASSERT_TRUE(!IsRegularMode(0xA1FF));
}


namespace {

struct BakedVolume {
    std::vector<std::uint8_t>                  image;
    std::unique_ptr<cinux::test::ImageDisk>    disk;
    std::unique_ptr<cinux::driver::BlockQueue> queue;
    std::unique_ptr<cinux::driver::BlockCache> cache;
    std::unique_ptr<cinux::fs::ext2::Ext2>     volume;
    std::vector<std::uint8_t>                  small;
    std::vector<std::uint8_t>                  single;
    std::vector<std::uint8_t>                  dbl;
    bool                                       ok    = false;
    bool                                       tried = false;
};

/// A frozen wall clock: every fresh timestamp reads the same value, so
/// the tests assert stamps without racing a real clock.
unsigned long long frozen_clock() {
    return 1893456000ULL;
}

bool run_quietly(const std::string& cmd) {
    // NOLINTNEXTLINE(bugprone-command-processor)
    if (std::system((cmd + " >/dev/null 2>&1").c_str()) != 0) {
        std::println(stderr, "[bake] command failed: {}", cmd);
        return false;
    }
    return true;
}

std::uint32_t peek_u32(const std::vector<std::uint8_t>& image, unsigned long byte_off) {
    return cinux::base::ReadWord32(image.data(), static_cast<std::uint32_t>(byte_off));
}

std::uint16_t peek_u16(const std::vector<std::uint8_t>& image, unsigned long byte_off) {
    return cinux::base::ReadWord16(image.data(), static_cast<std::uint32_t>(byte_off));
}

void poke_u32(std::vector<std::uint8_t>& image, unsigned long byte_off, std::uint32_t value) {
    cinux::base::WriteWord32(image.data(), static_cast<std::uint32_t>(byte_off), value);
}

void poke_u16(std::vector<std::uint8_t>& image, unsigned long byte_off, std::uint16_t value) {
    cinux::base::WriteWord16(image.data(), static_cast<std::uint32_t>(byte_off), value);
}

std::vector<std::uint8_t> pattern_bytes(unsigned long count) {
    std::vector<std::uint8_t> bytes(count);
    for (unsigned long i = 0; i < count; ++i) {
        bytes[i] = static_cast<std::uint8_t>(((i * 7U) + 3U) % 251U);
    }
    return bytes;
}

unsigned long inode_word_on_disk(cinux::test::ImageDisk& disk, unsigned long table_block,
                                 unsigned long byte_off) {
    cinux::driver::BlockQueue queue(disk);
    cinux::driver::BlockCache cache(queue, 4);
    const std::uint8_t*       view = nullptr;
    if (!cache.view(cinux::driver::Lba{.value = (table_block * 2) + (byte_off / 512)}, &view)) {
        return 0;
    }
    return cinux::base::ReadWord16(view, static_cast<std::uint32_t>(byte_off % 512));
}

unsigned long inode_dword_on_disk(cinux::test::ImageDisk& disk, unsigned long table_block,
                                  unsigned long byte_off) {
    cinux::driver::BlockQueue queue(disk);
    cinux::driver::BlockCache cache(queue, 4);
    const std::uint8_t*       view = nullptr;
    if (!cache.view(cinux::driver::Lba{.value = (table_block * 2) + (byte_off / 512)}, &view)) {
        return 0;
    }
    return cinux::base::ReadWord32(view, static_cast<std::uint32_t>(byte_off % 512));
}

bool inode_bit_clear_on_disk(cinux::test::ImageDisk& disk, const std::vector<std::uint8_t>& image,
                             unsigned long ino) {
    const unsigned long       kBitmapBlock = peek_u32(image, (2 * 1024) + 4);
    const unsigned long       kBit         = (ino - 1) % peek_u32(image, 1024 + 40);
    cinux::driver::BlockQueue queue(disk);
    cinux::driver::BlockCache cache(queue, 8);
    const std::uint8_t*       view = nullptr;
    if (!cache.view(cinux::driver::Lba{.value = kBitmapBlock * 2}, &view)) {
        return false;
    }
    return (view[kBit / 8] & (1U << (kBit % 8U))) == 0;
}

bool lists_a_dot_first(cinux::fs::ext2::Ext2& volume, const char* name) {
    const auto kNode = volume.lookup(name);
    if (!kNode.ok() || kNode.value().type != cinux::fs::InodeType::kDirectory) {
        return false;
    }
    cinux::fs::FsDirent first{};
    const auto          kFirst = volume.read_dir(kNode.value(), 0, first);
    return kFirst.ok() && kFirst.value() && first.name == std::string(".");
}

bool inode_freed_on_disk(cinux::test::ImageDisk& disk, const std::vector<std::uint8_t>& image,
                         unsigned long ino, unsigned long table) {
    const unsigned long kInodeSize = peek_u16(image, 1024 + 88);
    const unsigned long kStampOff  = ((ino - 1) * kInodeSize) + 20;
    if (inode_dword_on_disk(disk, table, kStampOff) != frozen_clock()) {
        return false;
    }
    return inode_bit_clear_on_disk(disk, image, ino);
}

bool write_file(const std::string& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(file);
}

BakedVolume& baked() {
    static BakedVolume baked_once;
    if (baked_once.ok || baked_once.tried) {
        return baked_once;
    }
    baked_once.tried = true;

    const std::string kDir   = "/tmp/cinux_ext2_test";
    const std::string kImage = kDir + "/volume.img";
    run_quietly("mkdir -p " + kDir);

    baked_once.small  = pattern_bytes(40);
    baked_once.single = pattern_bytes(20_KiB);
    baked_once.dbl    = pattern_bytes(525_KiB);
    if (!write_file(kDir + "/small.txt", baked_once.small) ||
        !write_file(kDir + "/single.bin", baked_once.single) ||
        !write_file(kDir + "/double.bin", baked_once.dbl)) {
        return baked_once;
    }

    const std::string kMake =
        "truncate -s 4M " + kImage + " && mkfs.ext2 -q -F -b 1024 -O none -N 128 " + kImage;
    if (!run_quietly(kMake)) {
        return baked_once;
    }
    const std::vector<std::string> kPuts = {"small.txt", "single.bin", "double.bin"};
    for (const std::string& name : kPuts) {
        std::string put = "debugfs -w -R 'write ";
        put += kDir;
        put += '/';
        put += name;
        put += ' ';
        put += name;
        put += "' ";
        put += kImage;
        if (!run_quietly(put)) {
            return baked_once;
        }
    }

    const std::vector<std::string> kLinks = {
        "symlink relative small.txt",
        "symlink absolute /small.txt",
        "symlink chained relative",
        "symlink directory /",
        "symlink broken absent",
        "symlink cycle cycle",
        "symlink longlink ././././././././././././././././././././././././././././././small.txt",
    };
    for (const auto& command : kLinks) {
        std::string invocation = "debugfs -w -R '";
        invocation += command;
        invocation += "' ";
        invocation += kImage;
        if (!run_quietly(invocation)) {
            return baked_once;
        }
    }

    std::ifstream file(kImage, std::ios::binary);
    if (!file) {
        return baked_once;
    }
    file.seekg(0, std::ios::end);
    const std::streamoff kBytes = file.tellg();
    file.seekg(0);
    baked_once.image.resize(static_cast<unsigned long>(kBytes));
    file.read(reinterpret_cast<char*>(baked_once.image.data()), kBytes);
    if (file.gcount() != kBytes) {
        return baked_once;
    }
    run_quietly("rm -rf " + kDir);

    baked_once.disk   = std::make_unique<cinux::test::ImageDisk>(baked_once.image);
    baked_once.queue  = std::make_unique<cinux::driver::BlockQueue>(*baked_once.disk);
    baked_once.cache  = std::make_unique<cinux::driver::BlockCache>(*baked_once.queue, 8);
    baked_once.volume = std::make_unique<cinux::fs::ext2::Ext2>(*baked_once.cache, frozen_clock);
    baked_once.ok     = baked_once.volume->mount().ok();
    return baked_once;
}

bool contains_name(const std::vector<std::string>& names, const char* wanted) {
    for (const std::string& name : names) {
        if (name == wanted) {
            return true;
        }
    }
    return false;
}

std::vector<std::string> list_root(cinux::fs::ext2::Ext2& volume, const cinux::fs::FsNode& root) {
    std::vector<std::string> names;
    for (unsigned long index = 0;; ++index) {
        cinux::fs::FsDirent entry{};
        const auto          kEntry = volume.read_dir(root, index, entry);
        if (!kEntry.ok() || !kEntry.value()) {
            return names;
        }
        names.emplace_back(entry.name);
    }
}

unsigned long root_first_block(const std::vector<std::uint8_t>& image) {
    const unsigned long kTable     = peek_u32(image, (2 * 1024) + 8);
    const auto          kInodeSize = static_cast<std::uint16_t>(
        static_cast<unsigned>(image[1024 + 88]) | (static_cast<unsigned>(image[1024 + 89]) << 8U));
    const unsigned long kRootOff = (kTable * 1024) + kInodeSize;
    std::uint32_t       first    = 0;
    for (unsigned long i = 0; i < 4; ++i) {
        first |= static_cast<std::uint32_t>(image[kRootOff + 40 + i]) << (8U * i);
    }
    return first;
}

unsigned long entry_offset_of(const std::vector<std::uint8_t>& image, unsigned long dir_block,
                              unsigned long ordinal) {
    unsigned long cursor = 0;
    unsigned long seen   = 0;
    while (cursor + 8 <= 1024) {
        const unsigned long kEntryBase = (dir_block * 1024) + cursor;
        const auto          kRec =
            static_cast<std::uint16_t>(static_cast<unsigned>(image[kEntryBase + 4]) |
                                       (static_cast<unsigned>(image[kEntryBase + 5]) << 8U));
        const std::uint32_t kIno = peek_u32(image, kEntryBase);
        if (kIno != 0) {
            if (seen == ordinal) {
                return kEntryBase;
            }
            ++seen;
        }
        cursor += kRec;
    }
    return 0;
}

unsigned long ordinal_of(const std::vector<std::string>& names, const char* wanted) {
    for (unsigned long i = 0; i < names.size(); ++i) {
        if (names[i] == wanted) {
            return i;
        }
    }
    return names.size();
}

struct StretchedMount {
    std::unique_ptr<cinux::fs::ext2::Ext2> volume;
    bool                                   ok = false;
};

StretchedMount mount_with_stretched_tail_name() {
    const BakedVolume& bake = baked();
    if (!bake.ok) {
        return {};
    }
    const auto                     kRoot   = bake.volume->lookup("");
    const std::vector<std::string> kNames  = list_root(*bake.volume, kRoot.value());
    const unsigned long            kTailAt = ordinal_of(kNames, "double.bin");
    if (kTailAt >= kNames.size()) {
        return {};
    }

    std::vector<std::uint8_t> stretched     = bake.image;
    const unsigned long       kRootDirBlock = root_first_block(stretched);
    const unsigned long       kTailEntryOff = entry_offset_of(stretched, kRootDirBlock, kTailAt);
    if (kTailEntryOff == 0) {
        return {};
    }
    stretched[kTailEntryOff + 6] = 40;
    stretched[kTailEntryOff + 7] = 0;

    static cinux::test::ImageDisk    disk(std::move(stretched));
    static cinux::driver::BlockQueue queue(disk);
    static cinux::driver::BlockCache cache(queue, 8);
    auto                             volume = std::make_unique<cinux::fs::ext2::Ext2>(cache);
    const bool                       kOk    = volume->mount().ok();
    return {.volume = std::move(volume), .ok = kOk};
}

}  // namespace

TEST("ext2: a baked mkfs volume mounts and reads a small file") {
    BakedVolume& bake = baked();
    ASSERT_TRUE(bake.ok);

    const auto kNode = bake.volume->lookup("small.txt");
    ASSERT_TRUE(kNode.ok());
    ASSERT_TRUE(kNode.value().type == cinux::fs::InodeType::kFile);
    ASSERT_TRUE(kNode.value().size == bake.small.size());

    std::vector<std::uint8_t> back(bake.small.size());
    const auto                kRead = bake.volume->read(kNode.value(), 0, back.data(), back.size());
    ASSERT_TRUE(kRead.ok());
    ASSERT_TRUE(kRead.value() == bake.small.size());
    for (unsigned long i = 0; i < bake.small.size(); ++i) {
        ASSERT_TRUE(back[i] == bake.small[i]);
    }
}

TEST("ext2: a twenty-KiB file walks the single-indirect layer") {
    BakedVolume& bake = baked();
    ASSERT_TRUE(bake.ok);

    const auto kNode = bake.volume->lookup("single.bin");
    ASSERT_TRUE(kNode.ok());
    ASSERT_TRUE(kNode.value().size == bake.single.size());

    std::vector<std::uint8_t> back(bake.single.size());
    const auto                kRead = bake.volume->read(kNode.value(), 0, back.data(), back.size());
    ASSERT_TRUE(kRead.ok());
    ASSERT_TRUE(kRead.value() == bake.single.size());
    for (unsigned long i = 0; i < bake.single.size(); ++i) {
        ASSERT_TRUE(back[i] == bake.single[i]);
    }
}

TEST("ext2: a three-hundred-KiB file walks the double-indirect layer") {
    BakedVolume& bake = baked();
    ASSERT_TRUE(bake.ok);

    const auto kNode = bake.volume->lookup("double.bin");
    ASSERT_TRUE(kNode.ok());
    ASSERT_TRUE(kNode.value().size == bake.dbl.size());

    std::vector<std::uint8_t> back(bake.dbl.size());
    const auto                kRead = bake.volume->read(kNode.value(), 0, back.data(), back.size());
    ASSERT_TRUE(kRead.ok());
    ASSERT_TRUE(kRead.value() == bake.dbl.size());
    for (unsigned long i = 0; i < bake.dbl.size(); ++i) {
        ASSERT_TRUE(back[i] == bake.dbl[i]);
    }
}

TEST("ext2: the root directory lists the baker's entries") {
    BakedVolume& bake = baked();
    ASSERT_TRUE(bake.ok);

    const auto kRoot = bake.volume->lookup("");
    ASSERT_TRUE(kRoot.ok());
    ASSERT_TRUE(kRoot.value().type == cinux::fs::InodeType::kDirectory);

    const std::vector<std::string> kNames = list_root(*bake.volume, kRoot.value());
    ASSERT_TRUE(contains_name(kNames, "."));
    ASSERT_TRUE(contains_name(kNames, "lost+found"));
    ASSERT_TRUE(contains_name(kNames, "small.txt"));
}

TEST("ext2: a missing name answers NotFound") {
    BakedVolume& bake = baked();
    ASSERT_TRUE(bake.ok);

    const auto kMissing = bake.volume->lookup("no-such-file");
    ASSERT_TRUE(!kMissing.ok());
    ASSERT_TRUE(kMissing.error() == cinux::base::KernelError::kNotFound);
}

TEST("ext2: a fresh file takes bytes and grows") {
    BakedVolume& bake = baked();
    ASSERT_TRUE(bake.ok);

    const auto kMade = bake.volume->make_file("written.txt");
    ASSERT_TRUE(kMade.ok());
    ASSERT_TRUE(kMade.value().type == cinux::fs::InodeType::kFile);
    ASSERT_TRUE(kMade.value().size == 0);

    const std::vector<std::uint8_t> kPayload = pattern_bytes(1000);
    const auto kStored = bake.volume->write(kMade.value(), 0, kPayload.data(), kPayload.size());
    ASSERT_TRUE(kStored.ok());
    ASSERT_TRUE(kStored.value() == kPayload.size());

    const auto kNode = bake.volume->lookup("written.txt");
    ASSERT_TRUE(kNode.ok());
    ASSERT_TRUE(kNode.value().size == kPayload.size());
}

TEST("ext2: a written file reads back its bytes") {
    BakedVolume& bake = baked();
    ASSERT_TRUE(bake.ok);

    const auto kNode = bake.volume->lookup("written.txt");
    ASSERT_TRUE(kNode.ok());

    const std::vector<std::uint8_t> kPayload = pattern_bytes(1000);
    std::vector<std::uint8_t>       back(kPayload.size());
    const auto kRead = bake.volume->read(kNode.value(), 0, back.data(), back.size());
    ASSERT_TRUE(kRead.ok());
    ASSERT_TRUE(cinux::base::EqualBytes(back.data(), kPayload.data(), back.size()));
}

TEST("ext2: stat reports the platter's own metadata") {
    BakedVolume& bake = baked();
    ASSERT_TRUE(bake.ok);

    const auto kNode = bake.volume->lookup("small.txt");
    ASSERT_TRUE(kNode.ok());
    cinux::fs::FsStat probed{};
    ASSERT_TRUE(bake.volume->stat(kNode.value(), probed).ok());
    ASSERT_TRUE(probed.size == kNode.value().size);
    ASSERT_TRUE(probed.mode == 0100644);
    ASSERT_TRUE(probed.links == 1);
    ASSERT_TRUE(probed.mtime > 0);
}

TEST("ext2: fresh nodes carry the injected clock's stamp") {
    BakedVolume& bake = baked();
    ASSERT_TRUE(bake.ok);

    const auto kMade = bake.volume->make_file("stamped.txt");
    ASSERT_TRUE(kMade.ok());
    cinux::fs::FsStat probed{};
    ASSERT_TRUE(bake.volume->stat(kMade.value(), probed).ok());
    ASSERT_TRUE(probed.mtime == frozen_clock());
    ASSERT_TRUE(probed.links == 1);
}

TEST("ext2: a twenty-KiB write crosses into the single-indirect table") {
    BakedVolume& bake = baked();
    ASSERT_TRUE(bake.ok);

    const auto kMade = bake.volume->make_file("twenty-kib.txt");
    ASSERT_TRUE(kMade.ok());
    const std::vector<std::uint8_t> kPayload = pattern_bytes(20_KiB);
    ASSERT_TRUE(bake.volume->write(kMade.value(), 0, kPayload.data(), kPayload.size()).ok());

    const auto kNode = bake.volume->lookup("twenty-kib.txt");
    ASSERT_TRUE(kNode.ok());
    ASSERT_TRUE(kNode.value().size == kPayload.size());
    std::vector<std::uint8_t> back(kPayload.size());
    ASSERT_TRUE(bake.volume->read(kNode.value(), 0, back.data(), back.size()).ok());
    ASSERT_TRUE(cinux::base::EqualBytes(back.data(), kPayload.data(), back.size()));
    ASSERT_TRUE(bake.volume->unlink("twenty-kib.txt").ok());
}

TEST("ext2: a three-hundred-KiB write crosses into the double-indirect tables") {
    BakedVolume& bake = baked();
    ASSERT_TRUE(bake.ok);

    const auto kMade = bake.volume->make_file("deep.bin");
    ASSERT_TRUE(kMade.ok());
    const std::vector<std::uint8_t> kPayload = pattern_bytes(300_KiB);
    ASSERT_TRUE(bake.volume->write(kMade.value(), 0, kPayload.data(), kPayload.size()).ok());

    const auto kNode = bake.volume->lookup("deep.bin");
    ASSERT_TRUE(kNode.ok());
    ASSERT_TRUE(kNode.value().size == kPayload.size());
    std::vector<std::uint8_t> back(kPayload.size());
    ASSERT_TRUE(bake.volume->read(kNode.value(), 0, back.data(), back.size()).ok());
    ASSERT_TRUE(cinux::base::EqualBytes(back.data(), kPayload.data(), back.size()));
    ASSERT_TRUE(bake.volume->unlink("deep.bin").ok());
}

TEST("ext2: a made directory links its parent and lists") {
    BakedVolume& bake = baked();
    ASSERT_TRUE(bake.ok);

    const unsigned long kTable     = peek_u32(bake.image, (2 * 1024) + 8);
    const unsigned long kInodeSize = peek_u16(bake.image, 1024 + 88);
    const auto          kRoot      = bake.volume->lookup("");
    ASSERT_TRUE(kRoot.ok());
    const unsigned long kLinksOff    = ((kRoot.value().ino - 1) * kInodeSize) + 26;
    const unsigned long kLinksBefore = peek_u16(bake.image, (kTable * 1024) + kLinksOff);

    const auto kMade = bake.volume->make_dir("made-dir");
    ASSERT_TRUE(kMade.ok());
    ASSERT_TRUE(bake.cache->flush());
    ASSERT_TRUE(inode_word_on_disk(*bake.disk, kTable, kLinksOff) == kLinksBefore + 1);
    ASSERT_TRUE(lists_a_dot_first(*bake.volume, "made-dir"));
}

TEST("ext2: an unlinked directory restores the parent and frees its inode") {
    BakedVolume& bake = baked();
    ASSERT_TRUE(bake.ok);

    const unsigned long kTable     = peek_u32(bake.image, (2 * 1024) + 8);
    const unsigned long kInodeSize = peek_u16(bake.image, 1024 + 88);
    const auto          kRoot      = bake.volume->lookup("");
    ASSERT_TRUE(kRoot.ok());
    const unsigned long kLinksOff       = ((kRoot.value().ino - 1) * kInodeSize) + 26;
    const unsigned long kLinksWhileHeld = inode_word_on_disk(*bake.disk, kTable, kLinksOff);

    const auto kLive = bake.volume->lookup("made-dir");
    ASSERT_TRUE(kLive.ok());

    const auto kRemoved = bake.volume->unlink("made-dir");
    ASSERT_TRUE(kRemoved.ok());
    ASSERT_TRUE(!bake.volume->lookup("made-dir").ok());
    ASSERT_TRUE(bake.cache->flush());
    ASSERT_TRUE(inode_word_on_disk(*bake.disk, kTable, kLinksOff) == kLinksWhileHeld - 1);
    ASSERT_TRUE(inode_freed_on_disk(*bake.disk, bake.image, kLive.value().ino, kTable));
}

TEST("ext2: without a clock deletion falls back to the volume's birth") {
    const BakedVolume& bake = baked();
    ASSERT_TRUE(bake.ok);

    cinux::test::ImageDisk    spare(bake.image);
    cinux::driver::BlockQueue spare_queue(spare);
    cinux::driver::BlockCache spare_cache(spare_queue, 8);
    cinux::fs::ext2::Ext2     clockless(spare_cache);
    ASSERT_TRUE(clockless.mount().ok());

    const auto kMade = clockless.make_dir("clockless-dir");
    ASSERT_TRUE(kMade.ok());
    const auto kGone = clockless.unlink("clockless-dir");
    ASSERT_TRUE(kGone.ok());
    ASSERT_TRUE(spare_cache.flush());

    const unsigned long       kTable     = peek_u32(bake.image, (2 * 1024) + 8);
    const unsigned long       kInodeSize = peek_u16(bake.image, 1024 + 88);
    const unsigned long       kStampOff  = ((kMade.value().ino - 1) * kInodeSize) + 20;
    cinux::driver::BlockQueue probe_queue(spare);
    cinux::driver::BlockCache probe_cache(probe_queue, 4);
    const std::uint8_t*       probe_view = nullptr;
    ASSERT_TRUE(probe_cache.view(cinux::driver::Lba{.value = (kTable * 2) + (kStampOff / 512)},
                                 &probe_view));
    const unsigned long kStamp =
        cinux::base::ReadWord32(probe_view, static_cast<std::uint32_t>(kStampOff % 512));
    ASSERT_TRUE(kStamp == peek_u32(bake.image, 1024 + 264));
    ASSERT_TRUE(kStamp != 0);
}

TEST("ext2: the superblock magic survives the write session") {
    BakedVolume& bake = baked();
    ASSERT_TRUE(bake.ok);

    const auto kFile = bake.volume->make_file("probe.txt");
    ASSERT_TRUE(kFile.ok());
    const std::vector<std::uint8_t> kPayload = pattern_bytes(1000);
    const auto kStored = bake.volume->write(kFile.value(), 0, kPayload.data(), kPayload.size());
    ASSERT_TRUE(kStored.ok());

    const auto kDir = bake.volume->make_dir("probe-dir");
    ASSERT_TRUE(kDir.ok());

    cinux::driver::BlockQueue fresh_queue(*bake.disk);
    cinux::driver::BlockCache fresh_cache(fresh_queue, 8);
    const std::uint8_t*       block_view = nullptr;
    ASSERT_TRUE(fresh_cache.view(cinux::driver::Lba{.value = 2}, &block_view));
    const unsigned kMagic =
        static_cast<unsigned>(block_view[56]) | (static_cast<unsigned>(block_view[57]) << 8U);
    ASSERT_TRUE(kMagic == 0xEF53U);
}

TEST("ext2: mount refuses a volume that is not ext2") {
    std::vector<std::uint8_t> zeros(4_MiB, 0);
    cinux::test::ImageDisk    disk(std::move(zeros));
    cinux::driver::BlockQueue queue(disk);
    cinux::driver::BlockCache cache(queue, 8);
    cinux::fs::ext2::Ext2     volume(cache);
    const auto                kMount = volume.mount();
    ASSERT_TRUE(!kMount.ok());
    ASSERT_TRUE(kMount.error() == cinux::base::KernelError::kIOError);
}


TEST("ext2: a five-hundred-KiB file crosses into the second double table") {
    BakedVolume& bake = baked();
    ASSERT_TRUE(bake.ok);

    const auto kNode = bake.volume->lookup("double.bin");
    ASSERT_TRUE(kNode.ok());
    ASSERT_TRUE(kNode.value().size == bake.dbl.size());

    std::vector<std::uint8_t> back(bake.dbl.size());
    const auto                kRead = bake.volume->read(kNode.value(), 0, back.data(), back.size());
    ASSERT_TRUE(kRead.ok());
    ASSERT_TRUE(kRead.value() == bake.dbl.size());
    for (unsigned long i = 0; i < bake.dbl.size(); ++i) {
        ASSERT_TRUE(back[i] == bake.dbl[i]);
    }
}

TEST("ext2: reads at unaligned offsets and clamped tails") {
    BakedVolume& bake = baked();
    ASSERT_TRUE(bake.ok);

    const auto kNode = bake.volume->lookup("small.txt");
    ASSERT_TRUE(kNode.ok());

    std::vector<std::uint8_t> window(10, 0xAA);
    const auto kWindow = bake.volume->read(kNode.value(), 5, window.data(), window.size());
    ASSERT_TRUE(kWindow.ok());
    ASSERT_TRUE(kWindow.value() == 10);
    for (unsigned long i = 0; i < 10; ++i) {
        ASSERT_TRUE(window[i] == bake.small[5 + i]);
    }
}

TEST("ext2: a read past the end clamps and then returns zero") {
    BakedVolume& bake = baked();
    ASSERT_TRUE(bake.ok);

    const auto kNode = bake.volume->lookup("small.txt");
    ASSERT_TRUE(kNode.ok());

    std::vector<std::uint8_t> tail(64, 0xAA);
    const auto kTail = bake.volume->read(kNode.value(), 36, tail.data(), tail.size());
    ASSERT_TRUE(kTail.ok());
    ASSERT_TRUE(kTail.value() == bake.small.size() - 36);

    const auto kPast = bake.volume->read(kNode.value(), bake.small.size(), tail.data(), 8);
    ASSERT_TRUE(kPast.ok());
    ASSERT_TRUE(kPast.value() == 0);
}

TEST("ext2: the multi-block lost+found directory walks across blocks") {
    BakedVolume& bake = baked();
    ASSERT_TRUE(bake.ok);

    const auto kDir = bake.volume->lookup("lost+found");
    ASSERT_TRUE(kDir.ok());
    ASSERT_TRUE(kDir.value().type == cinux::fs::InodeType::kDirectory);

    const std::vector<std::string> kNames = list_root(*bake.volume, kDir.value());
    ASSERT_TRUE(contains_name(kNames, "."));
    ASSERT_TRUE(contains_name(kNames, ".."));
}

TEST("ext2: a file node refuses directory questions and vice versa") {
    BakedVolume& bake = baked();
    ASSERT_TRUE(bake.ok);

    const auto kFile = bake.volume->lookup("small.txt");
    ASSERT_TRUE(kFile.ok());
    cinux::fs::FsDirent entry{};
    const auto          kDirRead = bake.volume->read_dir(kFile.value(), 0, entry);
    ASSERT_TRUE(!kDirRead.ok());
    ASSERT_TRUE(kDirRead.error() == cinux::base::KernelError::kNotADirectory);

    const auto kDir = bake.volume->lookup("");
    ASSERT_TRUE(kDir.ok());
    std::vector<std::uint8_t> few(4, 0);
    const auto kFileRead = bake.volume->read(kDir.value(), 0, few.data(), few.size());
    ASSERT_TRUE(!kFileRead.ok());
    ASSERT_TRUE(kFileRead.error() == cinux::base::KernelError::kIsADirectory);
}

TEST("ext2: faces before mount answer IOError") {
    std::vector<std::uint8_t> zeros(4_MiB, 1);
    cinux::test::ImageDisk    disk(std::move(zeros));
    cinux::driver::BlockQueue queue(disk);
    cinux::driver::BlockCache cache(queue, 8);
    cinux::fs::ext2::Ext2     unmounted(cache);

    const auto kLookup = unmounted.lookup("");
    ASSERT_TRUE(!kLookup.ok());
    ASSERT_TRUE(kLookup.error() == cinux::base::KernelError::kIOError);

    std::vector<std::uint8_t> few(4, 0);
    const auto                kNode = cinux::fs::FsNode{
        .opaque = nullptr,
        .type   = cinux::fs::InodeType::kFile,
        .ino    = 2,
        .size   = 4,
    };
    const auto kRead = unmounted.read(kNode, 0, few.data(), few.size());
    ASSERT_TRUE(!kRead.ok());
    ASSERT_TRUE(kRead.error() == cinux::base::KernelError::kIOError);

    cinux::fs::FsDirent entry{};
    const auto          kDir = unmounted.read_dir(kNode, 0, entry);
    ASSERT_TRUE(!kDir.ok());
    ASSERT_TRUE(kDir.error() == cinux::base::KernelError::kIOError);
}

TEST("ext2: mount refuses volumes with broken or unsupported shapes") {
    struct Breakage {
        unsigned long offset;
        std::uint32_t value;
        bool          as_u16;
    };
    const std::vector<Breakage> kBreaks = {
        {.offset = 1024 + 4, .value = 1, .as_u16 = false},
        {.offset = 1024 + 24, .value = 1, .as_u16 = false},
        {.offset = 1024 + 40, .value = 1, .as_u16 = false},
        {.offset = 1024 + 88, .value = 136, .as_u16 = true},
        {.offset = 1024 + 96, .value = 1, .as_u16 = false},
    };
    const BakedVolume& bake = baked();
    ASSERT_TRUE(bake.ok);
    for (const Breakage& entry : kBreaks) {
        std::vector<std::uint8_t> broken = bake.image;
        if (entry.as_u16) {
            poke_u16(broken, entry.offset, static_cast<std::uint16_t>(entry.value));
        } else {
            poke_u32(broken, entry.offset, entry.value);
        }

        cinux::test::ImageDisk    disk(std::move(broken));
        cinux::driver::BlockQueue queue(disk);
        cinux::driver::BlockCache cache(queue, 8);
        cinux::fs::ext2::Ext2     volume(cache);
        ASSERT_TRUE(!volume.mount().ok());
    }
}

TEST("ext2: a second mount is refused") {
    BakedVolume& bake = baked();
    ASSERT_TRUE(bake.ok);
    const auto kAgain = bake.volume->mount();
    ASSERT_TRUE(!kAgain.ok());
    ASSERT_TRUE(kAgain.error() == cinux::base::KernelError::kInvalidArgument);
}

namespace {

std::vector<std::uint8_t> holed_copy_of_small() {
    const BakedVolume& bake = baked();
    if (!bake.ok) {
        return {};
    }
    const auto kNode = bake.volume->lookup("small.txt");
    if (!kNode.ok()) {
        return {};
    }

    std::vector<std::uint8_t> holed      = bake.image;
    const unsigned long       kTable     = peek_u32(holed, (2 * 1024) + 8);
    const std::uint32_t       kIpg       = peek_u32(holed, 1024 + 40);
    const auto                kInodeSize = static_cast<std::uint16_t>(
        static_cast<unsigned>(holed[1024 + 88]) | (static_cast<unsigned>(holed[1024 + 89]) << 8U));
    const unsigned long kInodeOff =
        (kTable * 1024) + ((((kNode.value().ino - 1) % kIpg) * kInodeSize) + 40);
    poke_u32(holed, kInodeOff, 0);
    return holed;
}

}  // namespace

TEST("ext2: a holed file still reports its full size") {
    const BakedVolume& bake = baked();
    ASSERT_TRUE(bake.ok);
    const std::vector<std::uint8_t> kHoled = holed_copy_of_small();
    ASSERT_TRUE(!kHoled.empty());

    cinux::test::ImageDisk    disk(kHoled);
    cinux::driver::BlockQueue queue(disk);
    cinux::driver::BlockCache cache(queue, 8);
    cinux::fs::ext2::Ext2     volume(cache);
    ASSERT_TRUE(volume.mount().ok());

    const auto kHoleNode = volume.lookup("small.txt");
    ASSERT_TRUE(kHoleNode.ok());
    ASSERT_TRUE(kHoleNode.value().size == bake.small.size());
}

TEST("ext2: bytes of a holed file read back as zeros") {
    const BakedVolume& bake = baked();
    ASSERT_TRUE(bake.ok);
    const std::vector<std::uint8_t> kHoled = holed_copy_of_small();
    ASSERT_TRUE(!kHoled.empty());

    cinux::test::ImageDisk    disk(kHoled);
    cinux::driver::BlockQueue queue(disk);
    cinux::driver::BlockCache cache(queue, 8);
    cinux::fs::ext2::Ext2     volume(cache);
    ASSERT_TRUE(volume.mount().ok());

    const auto kHoleNode = volume.lookup("small.txt");
    ASSERT_TRUE(kHoleNode.ok());
    std::vector<std::uint8_t> back(bake.small.size(), 0xAA);
    const auto                kRead = volume.read(kHoleNode.value(), 0, back.data(), back.size());
    ASSERT_TRUE(kRead.ok());
    ASSERT_TRUE(kRead.value() == bake.small.size());
    for (const std::uint8_t kByte : back) {
        ASSERT_TRUE(kByte == 0);
    }
}

TEST("ext2: a stretched tail name disappears from the listing") {
    const StretchedMount kMount = mount_with_stretched_tail_name();
    ASSERT_TRUE(kMount.ok);

    const auto                     kRoot  = kMount.volume->lookup("");
    const std::vector<std::string> kNames = list_root(*kMount.volume, kRoot.value());
    ASSERT_TRUE(!contains_name(kNames, "double.bin"));
}

TEST("ext2: entries around a stretched name still list") {
    const StretchedMount kMount = mount_with_stretched_tail_name();
    ASSERT_TRUE(kMount.ok);

    const auto                     kRoot  = kMount.volume->lookup("");
    const std::vector<std::string> kNames = list_root(*kMount.volume, kRoot.value());
    ASSERT_TRUE(contains_name(kNames, "small.txt"));
    ASSERT_TRUE(contains_name(kNames, "single.bin"));
}

int main() {
    return cinux::test::RunAll();
}

namespace {

/// Writes the bytes to a scratch file and reports an e2fsck -fn verdict.
bool fsck_reports_clean(const std::string& path, const std::vector<std::uint8_t>& image) {
    // NOLINTNEXTLINE(bugprone-command-processor) the scratch tree is ours
    static_cast<void>(run_quietly("mkdir -p /tmp/cinux_ext2_faultsweep"));
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(image.data()),
              static_cast<std::streamsize>(image.size()));
    out.close();
    if (run_quietly("e2fsck -fn " + path)) {
        return true;
    }
    std::println(stderr, "[fsck] reporting on {}", path);
    const std::string          kDumpCmd      = "e2fsck -fn " + path + " 2>&1 | head -20";
    // NOLINTNEXTLINE(bugprone-command-processor) the scratch tree is ours
    [[maybe_unused]] const int kReportStatus = std::system(kDumpCmd.c_str());
    return false;
}

/// One staged rename setup: a source file and a victim to overwrite,
/// both with payloads no mistake can pass off as the other's.
struct RenameStage {
    std::vector<std::uint8_t> image;
    unsigned long             source_ino = 0;
    unsigned long             victim_ino = 0;
    unsigned long             writes     = 0;
};

// NOLINTNEXTLINE(readability-function-cognitive-complexity) a body walks one story in order
RenameStage prepare_rename_stage() {
    RenameStage stage;
    const auto& bake = baked();
    stage.image      = bake.image;
    cinux::test::ImageDisk    disk(stage.image);
    cinux::driver::BlockQueue queue(disk);
    cinux::driver::BlockCache cache(queue, 8);
    cinux::fs::ext2::Ext2     volume(cache, frozen_clock);
    if (!volume.mount().ok()) {
        return stage;
    }
    const auto kVictim = volume.make_file("victim");
    const auto kSource = volume.make_file("source");
    if (!kVictim.ok() || !kSource.ok()) {
        return stage;
    }
    stage.victim_ino = kVictim.value().ino;
    stage.source_ino = kSource.value().ino;
    const std::vector<std::uint8_t> kVictimBytes(64, 0xEE);
    const std::vector<std::uint8_t> kSourceBytes(64, 0x11);
    static_cast<void>(volume.write(kVictim.value(), 0, kVictimBytes.data(), kVictimBytes.size()));
    static_cast<void>(volume.write(kSource.value(), 0, kSourceBytes.data(), kSourceBytes.size()));
    static_cast<void>(cache.flush());
    stage.image  = disk.image();
    stage.writes = disk.write_count();
    return stage;
}

/// A crash on this unjournaled volume may leave bitmap and counter
/// drift; the honest claim is that fsck repairs it all automatically and
/// the data survives, so repair a copy, recheck it, and remount it.
// NOLINTNEXTLINE(bugprone-command-processor) the scratch tree is ours
bool crash_left_only_repairable_residue(const std::string&               path,
                                        const std::vector<std::uint8_t>& image) {
    // NOLINTNEXTLINE(bugprone-command-processor) the scratch tree is ours
    static_cast<void>(run_quietly("mkdir -p /tmp/cinux_ext2_faultsweep"));
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(image.data()),
              static_cast<std::streamsize>(image.size()));
    out.close();
    // NOLINTNEXTLINE(bugprone-command-processor) the scratch tree is ours
    static_cast<void>(run_quietly("e2fsck -fy " + path));
    return run_quietly("e2fsck -fn " + path);
}

/// Reports the inode number debugfs itself sees behind one name.
unsigned long debugfs_ino_of(const std::string& dir, const std::string& image_path,
                             const char* name) {
    run_quietly("cp " + image_path + " " + dir + "/probe.img");
    static_cast<void>(run_quietly("debugfs -R 'stat <" + std::string(name) + ">' " + dir +
                                  "/probe.img > " + dir + "/probe.stat"));
    std::ifstream report(dir + "/probe.stat");
    std::string   word;
    while (report >> word) {
        if (word == "Inode:") {
            unsigned long ino = 0;
            if (report >> ino) {
                return ino;
            }
        }
    }
    return 0;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) a body walks one story in order
TEST("ext2: a crash between the rename's writes never loses a named file") {
    const RenameStage kStage = prepare_rename_stage();
    ASSERT_TRUE(kStage.source_ino != 0);
    const std::string               kScratch = "/tmp/cinux_ext2_faultsweep/fault-sweep.img";
    const std::vector<std::uint8_t> kVictimBytes(64, 0xEE);
    const std::vector<std::uint8_t> kSourceBytes(64, 0x11);
    const unsigned long             kDirtyWritesForTheRename = 8;
    for (unsigned long cut = 0; cut <= kDirtyWritesForTheRename; ++cut) {
        cinux::test::ImageDisk    disk(kStage.image);
        cinux::driver::BlockQueue queue(disk);
        cinux::driver::BlockCache cache(queue, 8);
        cinux::fs::ext2::Ext2     volume(cache, frozen_clock);
        ASSERT_TRUE(volume.mount().ok());
        ASSERT_TRUE(volume.rename("source", "victim").ok());
        disk.fail_writes_from(cut);
        static_cast<void>(cache.flush());
        disk.fail_writes_from(~0UL);
        cinux::driver::BlockQueue cold_queue(disk);
        cinux::driver::BlockCache cold_cache(cold_queue, 8);
        cinux::fs::ext2::Ext2     cold(cold_cache, frozen_clock);
        const bool                kMounts = cold.mount().ok();
        ASSERT_TRUE(kMounts);
        const auto kOld            = cold.lookup("source");
        const auto kNew            = cold.lookup("victim");
        const bool kOldKeepsSource = kOld.ok() && kOld.value().ino == kStage.source_ino;
        const bool kNewKeepsVictim = kNew.ok() && kNew.value().ino == kStage.victim_ino;
        const bool kNewTookSource  = kNew.ok() && kNew.value().ino == kStage.source_ino;
        ASSERT_TRUE(kOldKeepsSource || kNewKeepsVictim || kNewTookSource);
        if (kNewKeepsVictim) {
            std::vector<std::uint8_t> read_back(64, 0);
            ASSERT_EQ(cold.read(kNew.value(), 0, read_back.data(), read_back.size()).value(), 64U);
            ASSERT_TRUE(read_back == kVictimBytes);
        }
        if (kNewTookSource) {
            std::vector<std::uint8_t> read_back(64, 0);
            ASSERT_EQ(cold.read(kNew.value(), 0, read_back.data(), read_back.size()).value(), 64U);
            ASSERT_TRUE(read_back == kSourceBytes);
            if (kOldKeepsSource) {
                ASSERT_EQ(debugfs_ino_of("/tmp/cinux_ext2_faultsweep", kScratch, "victim"),
                          kStage.source_ino);
            }
        }
        const bool kIntact = kOldKeepsSource && kNewKeepsVictim;
        const bool kMoved  = !kOld.ok() && kNewTookSource;
        if (kIntact || kMoved) {
            ASSERT_TRUE(crash_left_only_repairable_residue(kScratch, disk.image()));
        }
    }
}

}  // namespace

// NOLINTNEXTLINE(readability-function-cognitive-complexity) a body walks one story in order
// NOLINTNEXTLINE(readability-function-cognitive-complexity) a body walks one story in order
TEST("ext2: a held directory node anchors names while its inode stays distinct") {
    const auto& bake = baked();
    ASSERT_TRUE(bake.ok);
    cinux::test::ImageDisk    disk(bake.image);
    cinux::driver::BlockQueue queue(disk);
    cinux::driver::BlockCache cache(queue, 8);
    cinux::fs::ext2::Ext2     volume(cache, frozen_clock);
    ASSERT_TRUE(volume.mount().ok());
    ASSERT_TRUE(volume.make_dir("held").ok());
    ASSERT_TRUE(volume.make_file("held/first").ok());
    const auto kHeldNode = volume.lookup_held("held");
    ASSERT_TRUE(kHeldNode.ok());

    const auto kSameName = volume.lookup_at(kHeldNode.value(), "first");
    ASSERT_TRUE(kSameName.ok());
    ASSERT_EQ(kSameName.value().ino, volume.lookup("held/first").value().ino);
    volume.release(kSameName.value());

    ASSERT_TRUE(volume.unlink("held/first").ok());
    const auto kGone = volume.lookup_at(kHeldNode.value(), "first");
    ASSERT_TRUE(!kGone.ok());

    ASSERT_TRUE(volume.unlink("held").ok());
    ASSERT_TRUE(volume.make_file("recycle-seat").ok());
    ASSERT_TRUE(volume.make_dir("held").ok());
    ASSERT_TRUE(volume.make_file("held/first").ok());
    const auto kNewTree = volume.lookup("held/first");
    ASSERT_TRUE(kNewTree.ok());
    const auto kOldAnchor = volume.lookup_at(kHeldNode.value(), "first");
    ASSERT_TRUE(!kOldAnchor.ok());
    ASSERT_NE(kHeldNode.value().ino, volume.lookup("held").value().ino);
    volume.release(kHeldNode.value());
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) a body walks one story in order
TEST("ext2: a later group serves blocks on a real mke2fs multi-group volume") {
    const std::string kDir    = "/tmp/cinux_ext2_multigroup";
    const std::string kImage  = kDir + "/volume.img";
    const std::string kFiller = kDir + "/filler.bin";
    run_quietly("rm -rf " + kDir);
    run_quietly("mkdir -p " + kDir);
    const std::vector<std::uint8_t> kFillerBytes(7'000'000, 0x5A);
    ASSERT_TRUE(write_file(kFiller, kFillerBytes));
    std::string make = "truncate -s 16M ";
    make += kImage;
    make += " && mkfs.ext2 -q -F -b 1024 -O none -N 64 ";
    make += kImage;
    ASSERT_TRUE(run_quietly(make));
    std::string seat_filler = "debugfs -w -R 'write ";
    seat_filler += kFiller;
    seat_filler += " filler' ";
    seat_filler += kImage;
    ASSERT_TRUE(run_quietly(seat_filler));
    std::ifstream file(kImage, std::ios::binary);
    ASSERT_TRUE(file.good());
    file.seekg(0, std::ios::end);
    const std::streamoff kBytes = file.tellg();
    file.seekg(0);
    std::vector<std::uint8_t> image(static_cast<unsigned long>(kBytes));
    file.read(reinterpret_cast<char*>(image.data()), kBytes);
    ASSERT_EQ(file.gcount(), kBytes);
    cinux::test::ImageDisk    disk(std::move(image));
    cinux::driver::BlockQueue queue(disk);
    cinux::driver::BlockCache cache(queue, 8);
    cinux::fs::ext2::Ext2     volume(cache, frozen_clock);
    ASSERT_TRUE(volume.mount().ok());
    const auto kLate = volume.make_file("late.bin");
    ASSERT_TRUE(kLate.ok());
    const std::vector<std::uint8_t> kPayload(4096, 0xC3);
    ASSERT_EQ(volume.write(kLate.value(), 0, kPayload.data(), kPayload.size()).value(), 4096U);
    ASSERT_TRUE(cache.flush());
    std::string snapshot = "cp ";
    snapshot += kImage;
    snapshot += ' ';
    snapshot += kDir;
    snapshot += "/after.img";
    static_cast<void>(run_quietly(snapshot));
    std::string blocks_query = "debugfs -R 'blocks <late.bin>' ";
    blocks_query += kDir;
    blocks_query += "/after.img > ";
    blocks_query += kDir;
    blocks_query += "/late.blocks";
    ASSERT_TRUE(run_quietly(blocks_query));
    std::ifstream blocks_file(kDir + "/late.blocks");
    ASSERT_TRUE(blocks_file.good());
    unsigned long block_id = 0;
    unsigned long lowest   = ~0UL;
    while (blocks_file >> block_id) {
        lowest = block_id < lowest ? block_id : lowest;
    }
    ASSERT_TRUE(lowest >= 8192UL);
    ASSERT_TRUE(fsck_reports_clean(kDir + "/fsck.img", disk.image()));
    cinux::driver::BlockQueue cold_queue(disk);
    cinux::driver::BlockCache cold_cache(cold_queue, 8);
    cinux::fs::ext2::Ext2     cold(cold_cache, frozen_clock);
    ASSERT_TRUE(cold.mount().ok());
    const auto kColdLate = cold.lookup("late.bin");
    ASSERT_TRUE(kColdLate.ok());
    std::vector<std::uint8_t> read_back(4096, 0);
    ASSERT_EQ(cold.read(kColdLate.value(), 0, read_back.data(), read_back.size()).value(), 4096U);
    ASSERT_TRUE(read_back == kPayload);
    run_quietly("rm -rf " + kDir);
}

TEST("ext2: lookup follows relative absolute chained directory and long symlinks") {
    auto& bake = baked();
    ASSERT_TRUE(bake.ok);
    const auto kFile = bake.volume->lookup("small.txt");
    ASSERT_TRUE(kFile.ok());
    for (const auto* path :
         {"relative", "absolute", "chained", "directory/small.txt", "longlink"}) {
        const auto kNode = bake.volume->lookup(path);
        ASSERT_TRUE(kNode.ok());
        ASSERT_EQ(kNode.value().ino, kFile.value().ino);
    }
    ASSERT_FALSE(bake.volume->lookup("broken").ok());
    ASSERT_FALSE(bake.volume->lookup("cycle").ok());
}

namespace {

void check_ext2_truncate(cinux::fs::ext2::Ext2& volume, cinux::fs::FsNode node) {
    ASSERT_TRUE(volume.truncate(node).ok());
    char bytes[4]{};
    ASSERT_EQ(volume.read(node, 0, bytes, sizeof(bytes)).value(), 0UL);
    ASSERT_EQ(volume.write(node, 3, "z", 1).value(), 1UL);
    ASSERT_EQ(volume.read(node, 0, bytes, sizeof(bytes)).value(), 4UL);
    ASSERT_TRUE(std::string_view(bytes, 4) == std::string_view("\0\0\0z", 4));
}

}

TEST("ext2: append uses current size and truncate reclaims indirect blocks") {
    BakedVolume& bake = baked();
    ASSERT_TRUE(bake.ok);
    const auto kNode = bake.volume->make_file("append-truncate");
    ASSERT_TRUE(kNode.ok());
    const std::string kBytes(300UL * 1024, 'x');
    ASSERT_EQ(bake.volume->write(kNode.value(), 0, kBytes.data(), kBytes.size()).value(),
              kBytes.size());
    unsigned long cursor = 1;
    ASSERT_EQ(bake.volume->append(kNode.value(), cursor, "end", 3).value(), 3UL);
    ASSERT_EQ(cursor, kBytes.size() + 3);
    char bytes[4]{};
    ASSERT_EQ(bake.volume->read(kNode.value(), kBytes.size(), bytes, sizeof(bytes)).value(), 3UL);
    ASSERT_TRUE(std::string_view(bytes, 3) == "end");
    check_ext2_truncate(*bake.volume, kNode.value());
    ASSERT_TRUE(bake.cache->flush());
}

namespace {

void cache_read_tag(cinux::driver::BlockCache& cache, unsigned long page) {
    unsigned char byte{};
    ASSERT_TRUE(cache.read(cinux::driver::Lba{.value = page * 8}, &byte, 1));
    ASSERT_EQ(byte, static_cast<unsigned char>(page));
}

std::vector<std::uint8_t> tagged_cache_image(unsigned long pages) {
    std::vector<std::uint8_t> bytes(pages * 4096);
    for (unsigned long page = 0; page < pages; ++page) {
        bytes[page * 4096] = static_cast<unsigned char>(page);
    }
    return bytes;
}

void check_dirty_cache_failure(cinux::driver::BlockCache& cache, cinux::test::ImageDisk& disk) {
    const unsigned char kByte = 77;
    ASSERT_TRUE(cache.patch(cinux::driver::Lba{.value = 0}, 0, 1, &kByte));
    disk.fail_writes(true);
    unsigned char byte{};
    ASSERT_FALSE(cache.read(cinux::driver::Lba{.value = 8}, &byte, 1));
    ASSERT_TRUE(cache.read(cinux::driver::Lba{.value = 0}, &byte, 1));
    ASSERT_EQ(byte, kByte);
    ASSERT_FALSE(cache.flush());
    disk.fail_writes(false);
    ASSERT_TRUE(cache.flush());
}

std::string growing_directory_file(unsigned long index) {
    return "grow/" + std::string(230, 'x') + std::to_string(index);
}

void fill_growing_directory(cinux::fs::ext2::Ext2& volume) {
    ASSERT_TRUE(volume.make_dir("grow").ok());
    for (unsigned long index = 0; index < 56; ++index) {
        ASSERT_TRUE(volume.make_file(growing_directory_file(index)).ok());
    }
    ASSERT_TRUE(volume.make_dir(growing_directory_file(56)).ok());
    ASSERT_TRUE(volume.lookup(growing_directory_file(0)).ok());
    ASSERT_TRUE(volume.lookup(growing_directory_file(55)).ok());
    ASSERT_TRUE(volume.lookup(growing_directory_file(56)).ok());
}

void check_growing_directory(cinux::fs::ext2::Ext2& volume) {
    const auto kNode = volume.lookup("grow");
    ASSERT_TRUE(kNode.ok());
    cinux::fs::FsStat stat{};
    ASSERT_TRUE(volume.stat(kNode.value(), stat).ok());
    ASSERT_EQ(stat.size, 15UL * 1024);
    ASSERT_EQ(stat.links, 3UL);
    unsigned long       entries = 0;
    cinux::fs::FsDirent entry{};
    for (;;) {
        const auto kEntry = volume.read_dir(kNode.value(), entries, entry);
        ASSERT_TRUE(kEntry.ok());
        if (!kEntry.value()) {
            break;
        }
        ++entries;
    }
    ASSERT_EQ(entries, 59UL);
}

}

TEST("block cache: a warm working set larger than eight pages performs no device reads") {
    cinux::test::ImageDisk    disk(tagged_cache_image(64));
    cinux::driver::BlockQueue queue(disk);
    cinux::driver::BlockCache cache(queue, 32);
    for (unsigned long page = 0; page < 24; ++page) {
        cache_read_tag(cache, page);
    }
    const auto kReads = disk.read_count();
    for (unsigned long page = 0; page < 24; ++page) {
        cache_read_tag(cache, page);
    }
    ASSERT_EQ(kReads, 24UL);
    ASSERT_EQ(disk.read_count(), kReads);
}

TEST("block cache: hash collisions and second-chance eviction retain a reused page") {
    cinux::test::ImageDisk    disk(tagged_cache_image(16));
    cinux::driver::BlockQueue queue(disk);
    cinux::driver::BlockCache cache(queue, 3);
    for (const unsigned long kPage : {0UL, 7UL, 14UL, 1UL, 7UL, 2UL}) {
        cache_read_tag(cache, kPage);
    }
    const auto kReads = disk.read_count();
    cache_read_tag(cache, 7);
    ASSERT_EQ(disk.read_count(), kReads);
    cache_read_tag(cache, 0);
    ASSERT_EQ(disk.read_count(), kReads + 1);
}

TEST("block cache: failed writeback preserves dirty bytes and failed fills retry") {
    cinux::test::ImageDisk    disk(tagged_cache_image(2));
    cinux::driver::BlockQueue queue(disk);
    cinux::driver::BlockCache cache(queue, 1);
    check_dirty_cache_failure(cache, disk);
    disk.fail_reads(true);
    unsigned char byte{};
    ASSERT_FALSE(cache.read(cinux::driver::Lba{.value = 8}, &byte, 1));
    disk.fail_reads(false);
    cache_read_tag(cache, 1);
    ASSERT_TRUE(cache.read(cinux::driver::Lba{.value = 0}, &byte, 1));
    ASSERT_EQ(byte, 77);
}

namespace {

void check_directory_disk_blocks(BakedVolume& bake) {
    const auto kNode = bake.volume->lookup("grow");
    ASSERT_TRUE(kNode.ok());
    const auto kTable     = peek_u32(bake.image, (2 * 1024) + 8);
    const auto kInodeSize = peek_u16(bake.image, 1024 + 88);
    const auto kField     = ((kNode.value().ino - 1) * kInodeSize) + offsetof(Inode, i_blocks);
    ASSERT_EQ(inode_dword_on_disk(*bake.disk, kTable, kField), 32UL);
}

}

TEST("ext2: file and directory creation grow across direct blocks and persist") {
    BakedVolume& bake = baked();
    ASSERT_TRUE(bake.ok);
    fill_growing_directory(*bake.volume);
    check_growing_directory(*bake.volume);
    ASSERT_TRUE(bake.cache->flush());
    cinux::driver::BlockQueue queue(*bake.disk);
    cinux::driver::BlockCache cache(queue, 8);
    cinux::fs::ext2::Ext2     volume(cache);
    ASSERT_TRUE(volume.mount().ok());
    check_growing_directory(volume);
    check_directory_disk_blocks(bake);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) one allocation and remount scenario.
TEST("ext2: descriptors beyond the first table block allocate and persist") {
    const auto& bake = baked();
    ASSERT_TRUE(bake.ok);
    auto                    image      = bake.image;
    constexpr unsigned long kBlock     = 1024;
    constexpr unsigned long kGroup     = 32;
    const auto              kInodes    = peek_u32(image, kBlock + 40);
    const auto              kBlocks    = peek_u32(image, kBlock + 32);
    const auto              kBitmap    = peek_u32(image, 2 * kBlock);
    const auto              kRelocated = (image.size() / kBlock) - 1;
    cinux::base::CopyBytes(image.data() + (kRelocated * kBlock), image.data() + (kBitmap * kBlock),
                           kBlock);
    cinux::base::SetBytes(image.data() + (3 * kBlock), 0, kBlock);
    cinux::base::CopyBytes(image.data() + (3 * kBlock), image.data() + (2 * kBlock),
                           sizeof(GroupDescriptor));
    poke_u32(image, kBlock, (kGroup + 1) * kInodes);
    poke_u32(image, kBlock + 4, (kGroup * kBlocks) + 4096);
    poke_u32(image, 2 * kBlock, static_cast<std::uint32_t>(kRelocated));
    poke_u16(image, (2 * kBlock) + offsetof(GroupDescriptor, bg_free_inodes_count), 0);
    cinux::test::ImageDisk disk(std::move(image));
    {
        cinux::driver::BlockQueue queue(disk);
        cinux::driver::BlockCache cache(queue, 8);
        cinux::fs::ext2::Ext2     volume(cache);
        ASSERT_TRUE(volume.mount().ok());
        const auto kMade = volume.make_file("later-group");
        ASSERT_TRUE(kMade.ok());
        ASSERT_TRUE(kMade.value().ino > kGroup * kInodes);
        const unsigned char kBytes[] = {11, 22, 33};
        ASSERT_TRUE(volume.write(kMade.value(), 0, kBytes, sizeof(kBytes)).ok());
        ASSERT_TRUE(cache.flush());
    }
    cinux::driver::BlockQueue queue(disk);
    cinux::driver::BlockCache cache(queue, 8);
    cinux::fs::ext2::Ext2     volume(cache);
    ASSERT_TRUE(volume.mount().ok());
    const auto kFound = volume.lookup("later-group");
    ASSERT_TRUE(kFound.ok());
    unsigned char bytes[3]{};
    const auto    kRead = volume.read(kFound.value(), 0, bytes, sizeof(bytes));
    ASSERT_TRUE(kRead.ok());
    ASSERT_EQ(kRead.value(), sizeof(bytes));
    ASSERT_EQ(bytes[0], 11);
    ASSERT_EQ(bytes[2], 33);
    const auto kNext = volume.make_file("later-group-next");
    ASSERT_TRUE(kNext.ok());
    ASSERT_TRUE(kNext.value().ino != kFound.value().ino);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) one replace and remount scenario.
TEST("ext2: rename replaces a file and preserves inode permissions across remount") {
    const auto& bake = baked();
    ASSERT_TRUE(bake.ok);
    cinux::test::ImageDisk disk(bake.image);
    unsigned long          moved_ino = 0;
    {
        cinux::driver::BlockQueue queue(disk);
        cinux::driver::BlockCache cache(queue, 8);
        cinux::fs::ext2::Ext2     volume(cache);
        ASSERT_TRUE(volume.mount().ok());
        const auto kSource = volume.make_file("rename-source");
        ASSERT_TRUE(kSource.ok());
        moved_ino                      = kSource.value().ino;
        const unsigned char kPayload[] = {4, 5, 6};
        ASSERT_TRUE(volume.write(kSource.value(), 0, kPayload, sizeof(kPayload)).ok());
        ASSERT_TRUE(volume.make_file("rename-target").ok());
        ASSERT_TRUE(volume.chmod(kSource.value(), 0640).ok());
        ASSERT_TRUE(volume.rename("rename-source", "rename-target").ok());
        ASSERT_TRUE(!volume.lookup("rename-source").ok());
        ASSERT_TRUE(cache.flush());
    }
    cinux::driver::BlockQueue queue(disk);
    cinux::driver::BlockCache cache(queue, 8);
    cinux::fs::ext2::Ext2     volume(cache);
    ASSERT_TRUE(volume.mount().ok());
    const auto kMoved = volume.lookup("rename-target");
    ASSERT_TRUE(kMoved.ok());
    ASSERT_EQ(kMoved.value().ino, moved_ino);
    cinux::fs::FsStat facts{};
    ASSERT_TRUE(volume.stat(kMoved.value(), facts).ok());
    ASSERT_EQ(facts.mode & 0777U, 0640U);
    unsigned char bytes[3]{};
    ASSERT_TRUE(volume.read(kMoved.value(), 0, bytes, sizeof(bytes)).ok());
    ASSERT_EQ(bytes[0], 4);
    ASSERT_EQ(bytes[2], 6);
}

TEST("ext2: reusing a deleted entry preserves the next record with a different name length") {
    const auto& bake = baked();
    ASSERT_TRUE(bake.ok);
    auto       image  = bake.image;
    const auto kSmall = bake.volume->lookup("small.txt");
    ASSERT_TRUE(kSmall.ok());
    const auto kBlock = root_first_block(image) * 1024;
    cinux::base::SetBytes(image.data() + kBlock, 0, 1024);
    poke_u16(image, kBlock + 4, 512);
    image[kBlock + 6] = 100;
    poke_u32(image, kBlock + 512, static_cast<std::uint32_t>(kSmall.value().ino));
    poke_u16(image, kBlock + 516, 512);
    image[kBlock + 518] = 8;
    cinux::base::CopyBytes(image.data() + kBlock + 520, "sentinel", 8);
    cinux::test::ImageDisk    disk(std::move(image));
    cinux::driver::BlockQueue queue(disk);
    cinux::driver::BlockCache cache(queue, 4);
    cinux::fs::ext2::Ext2     volume(cache);
    ASSERT_TRUE(volume.mount().ok());
    ASSERT_TRUE(volume.make_file("new").ok());
    ASSERT_TRUE(volume.lookup("sentinel").ok());
    ASSERT_TRUE(volume.make_file("another-new-entry").ok());
    ASSERT_TRUE(volume.lookup("new").ok());
    ASSERT_TRUE(volume.lookup("sentinel").ok());
}

namespace {

void prepare_sync_file(cinux::fs::ext2::Ext2& volume, const std::vector<std::uint8_t>& payload) {
    ASSERT_TRUE(volume.make_dir("synced").ok());
    const auto kFile = volume.make_file("synced/pending");
    ASSERT_TRUE(kFile.ok());
    ASSERT_EQ(volume.write(kFile.value(), 0, payload.data(), payload.size()).value(),
              payload.size());
    ASSERT_TRUE(volume.rename("synced/pending", "synced/complete").ok());
    ASSERT_TRUE(volume.sync());
}

void verify_sync_file(cinux::fs::ext2::Ext2& cold, const std::vector<std::uint8_t>& payload) {
    ASSERT_FALSE(cold.lookup("synced/pending").ok());
    const auto kColdFile = cold.lookup("synced/complete");
    ASSERT_TRUE(kColdFile.ok());
    std::vector<std::uint8_t> back(payload.size());
    ASSERT_EQ(cold.read(kColdFile.value(), 0, back.data(), back.size()).value(), back.size());
    ASSERT_TRUE(back == payload);
}

}

TEST("ext2: volume sync persists namespace and indirect file data for a cold remount") {
    const BakedVolume& bake = baked();
    ASSERT_TRUE(bake.ok);
    cinux::test::ImageDisk    disk(bake.image);
    cinux::driver::BlockQueue queue(disk);
    cinux::driver::BlockCache cache(queue, 8);
    cinux::fs::ext2::Ext2     volume(cache);
    ASSERT_TRUE(volume.mount().ok());
    const auto kPayload = pattern_bytes(300_KiB);
    prepare_sync_file(volume, kPayload);
    ASSERT_EQ(disk.flush_count(), 1UL);
    cinux::driver::BlockQueue cold_queue(disk);
    cinux::driver::BlockCache cold_cache(cold_queue, 4);
    cinux::fs::ext2::Ext2     cold(cold_cache);
    ASSERT_TRUE(cold.mount().ok());
    verify_sync_file(cold, kPayload);
}

TEST("ext2 regression: a retained directory fd cannot turn into a recreated directory") {
    const auto& bake = baked();
    ASSERT_TRUE(bake.ok);
    cinux::test::ImageDisk    disk(bake.image);
    cinux::driver::BlockQueue queue(disk);
    cinux::driver::BlockCache cache(queue, 8);
    cinux::fs::ext2::Ext2     volume(cache, frozen_clock);
    ASSERT_TRUE(volume.mount().ok());
    const auto kHeld = volume.make_dir("review-held");
    ASSERT_TRUE(kHeld.ok());
    const auto kOpened = volume.lookup_held("review-held");
    ASSERT_TRUE(kOpened.ok());
    ASSERT_TRUE(volume.unlink("review-held").ok());
    const auto kRecreated = volume.make_dir("review-held");
    ASSERT_TRUE(kRecreated.ok());
    ASSERT_TRUE(volume.make_file("review-held/foreign").ok());
    const auto kForeign = volume.lookup_at(kHeld.value(), "foreign");
    std::println("[review] held inode={}, recreated inode={}, old handle reached new file={}",
                 kHeld.value().ino, kRecreated.value().ino, kForeign.ok());
    if (kForeign.ok()) {
        volume.release(kForeign.value());
    }
    volume.release(kHeld.value());
    ASSERT_FALSE(kForeign.ok());
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) lifetime regression.
TEST("ext2 regression: rename keeps an independently opened replaced target alive") {
    const auto& bake = baked();
    ASSERT_TRUE(bake.ok);
    cinux::test::ImageDisk    disk(bake.image);
    cinux::driver::BlockQueue queue(disk);
    cinux::driver::BlockCache cache(queue, 8);
    cinux::fs::ext2::Ext2     volume(cache, frozen_clock);
    ASSERT_TRUE(volume.mount().ok());
    const auto kSource = volume.make_file("review-source");
    const auto kTarget = volume.make_file("review-target");
    ASSERT_TRUE(kSource.ok() && kTarget.ok());
    ASSERT_TRUE(volume.write(kSource.value(), 0, "NEW", 3).ok());
    ASSERT_TRUE(volume.write(kTarget.value(), 0, "OLD", 3).ok());
    volume.retain(kTarget.value());
    ASSERT_TRUE(volume.rename("review-source", "review-target").ok());
    const auto kReplacement = volume.make_file("review-inode-reuse");
    ASSERT_TRUE(kReplacement.ok());
    ASSERT_TRUE(volume.write(kReplacement.value(), 0, "BAD", 3).ok());
    char       bytes[4]{};
    const auto kRead = volume.read(kTarget.value(), 0, bytes, 3);
    std::println("[review] replaced target inode={}, fresh file inode={}, held target bytes={}",
                 kTarget.value().ino, kReplacement.value().ino, bytes);
    volume.release(kTarget.value());
    ASSERT_TRUE(kRead.ok());
    ASSERT_TRUE(std::string_view(bytes, 3) == "OLD");
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) failure matrix.
TEST("ext2 regression: a failed rename preserves both preexisting names and contents") {
    const auto& bake = baked();
    ASSERT_TRUE(bake.ok);
    cinux::test::ImageDisk seed(bake.image);
    {
        cinux::driver::BlockQueue queue(seed);
        cinux::driver::BlockCache cache(queue, 16);
        cinux::fs::ext2::Ext2     volume(cache, frozen_clock);
        ASSERT_TRUE(volume.mount().ok());
        ASSERT_TRUE(volume.make_dir("review-from").ok());
        ASSERT_TRUE(volume.make_dir("review-to").ok());
        const auto kSource = volume.make_file("review-from/source");
        const auto kTarget = volume.make_file("review-to/target");
        ASSERT_TRUE(kSource.ok() && kTarget.ok());
        ASSERT_TRUE(volume.write(kSource.value(), 0, "NEW", 3).ok());
        ASSERT_TRUE(volume.write(kTarget.value(), 0, "OLD", 3).ok());
        ASSERT_TRUE(cache.flush());
    }
    bool broken = false;
    for (unsigned long slots = 1; slots <= 4 && !broken; ++slots) {
        for (unsigned long cut = 0; cut < 12 && !broken; ++cut) {
            cinux::test::ImageDisk    disk(seed.image());
            cinux::driver::BlockQueue queue(disk);
            cinux::driver::BlockCache cache(queue, slots);
            cinux::fs::ext2::Ext2     volume(cache, frozen_clock);
            ASSERT_TRUE(volume.mount().ok());
            disk.fail_writes_from(cut);
            const auto kResult = volume.rename("review-from/source", "review-to/target");
            disk.fail_writes_from(~0UL);
            if (kResult.ok()) {
                continue;
            }
            const auto kSource = volume.lookup("review-from/source");
            const auto kTarget = volume.lookup("review-to/target");
            char       source_bytes[4]{};
            char       target_bytes[4]{};
            if (kSource.ok()) {
                ASSERT_TRUE(volume.read(kSource.value(), 0, source_bytes, 3).ok());
            }
            if (kTarget.ok()) {
                ASSERT_TRUE(volume.read(kTarget.value(), 0, target_bytes, 3).ok());
            }
            broken = !kSource.ok() || !kTarget.ok() || std::string_view(source_bytes, 3) != "NEW" ||
                     std::string_view(target_bytes, 3) != "OLD";
            if (broken) {
                std::println(
                    "[review] rename returned error: slots={}, cut={}, source={}, target={}", slots,
                    cut, source_bytes, target_bytes);
            }
        }
    }
    ASSERT_FALSE(broken);
}

// NOLINTBEGIN(readability-function-cognitive-complexity) preparation fault matrix.
TEST(
    "ext2 regression: failed rename preparation preserves the original independently checked "
    "image") {
    const auto& bake = baked();
    ASSERT_TRUE(bake.ok);
    cinux::test::ImageDisk seed(bake.image);
    {
        cinux::driver::BlockQueue queue(seed);
        cinux::driver::BlockCache cache(queue, 16);
        cinux::fs::ext2::Ext2     volume(cache, frozen_clock);
        ASSERT_TRUE(volume.mount().ok());
        ASSERT_TRUE(volume.make_dir("from").ok());
        ASSERT_TRUE(volume.make_dir("to").ok());
        const auto kSource = volume.make_file("from/source");
        const auto kTarget = volume.make_file("to/target");
        ASSERT_TRUE(kSource.ok() && kTarget.ok());
        ASSERT_TRUE(volume.write(kSource.value(), 0, "NEW", 3).ok());
        ASSERT_TRUE(volume.write(kTarget.value(), 0, "OLD", 3).ok());
        ASSERT_TRUE(volume.sync());
    }
    unsigned long failures  = 0;
    unsigned long successes = 0;
    for (unsigned long cut = 0; cut < 20; ++cut) {
        cinux::test::ImageDisk    disk(seed.image());
        cinux::driver::BlockQueue queue(disk);
        cinux::driver::BlockCache cache(queue, 8);
        cinux::fs::ext2::Ext2     volume(cache, frozen_clock);
        ASSERT_TRUE(volume.mount().ok());
        disk.fail_reads_from(disk.read_count() + cut);
        const auto kMoved = volume.rename("from/source", "to/target");
        disk.fail_reads_from(~0UL);
        if (kMoved.ok()) {
            ++successes;
            continue;
        }
        ++failures;
        ASSERT_TRUE(volume.sync());
        ASSERT_TRUE(disk.image() == seed.image());
        ASSERT_TRUE(fsck_reports_clean("/tmp/cinux-review-failed-rename.img", disk.image()));
    }
    ASSERT_NE(failures, 0UL);
    ASSERT_NE(successes, 0UL);
}

// NOLINTEND(readability-function-cognitive-complexity)

// NOLINTNEXTLINE(readability-function-cognitive-complexity) orphan persistence scenario.
TEST("ext2 regression: deferred final close reclaims orphan blocks and deletion time") {
    const auto& bake = baked();
    ASSERT_TRUE(bake.ok);
    cinux::test::ImageDisk    disk(bake.image);
    cinux::driver::BlockQueue queue(disk);
    cinux::driver::BlockCache cache(queue, 8);
    cinux::fs::ext2::Ext2     volume(cache, frozen_clock);
    ASSERT_TRUE(volume.mount().ok());
    ASSERT_TRUE(volume.make_file("held-file").ok());
    const auto kHeld = volume.lookup_held("held-file");
    ASSERT_TRUE(kHeld.ok());
    ASSERT_TRUE(volume.write(kHeld.value(), 0, "OLD", 3).ok());
    ASSERT_TRUE(volume.unlink("held-file").ok());
    char bytes[3]{};
    ASSERT_TRUE(volume.read(kHeld.value(), 0, bytes, sizeof(bytes)).ok());
    ASSERT_TRUE(std::string_view(bytes, 3) == "OLD");
    const auto kWrites = disk.write_count();
    volume.release(kHeld.value());
    ASSERT_EQ(disk.write_count(), kWrites);
    ASSERT_TRUE(volume.sync());
    ASSERT_TRUE(fsck_reports_clean("/tmp/cinux-review-orphan.img", disk.image()));
}

TEST("block cache: an uncommittable batch leaves cached and platter bytes unchanged") {
    cinux::test::ImageDisk           disk(std::vector<std::uint8_t>(8192));
    cinux::driver::BlockQueue        queue(disk);
    cinux::driver::BlockCache        cache(queue, 1);
    cinux::driver::BlockCache::Batch batch(cache);
    const unsigned char              kByte = 17;
    ASSERT_TRUE(batch.patch(cinux::driver::Lba{0}, 0, 1, &kByte));
    ASSERT_TRUE(batch.patch(cinux::driver::Lba{8}, 0, 1, &kByte));
    ASSERT_FALSE(batch.commit());
    unsigned char back = 99;
    ASSERT_TRUE(cache.read(cinux::driver::Lba{0}, &back, 1));
    ASSERT_EQ(back, 0);
    ASSERT_TRUE(cache.read(cinux::driver::Lba{8}, &back, 1));
    ASSERT_EQ(back, 0);
    ASSERT_EQ(disk.write_count(), 0UL);
}

TEST("ext2 regression: a detached directory handle cannot create an unreachable child") {
    const auto& bake = baked();
    ASSERT_TRUE(bake.ok);
    cinux::test::ImageDisk    disk(bake.image);
    cinux::driver::BlockQueue queue(disk);
    cinux::driver::BlockCache cache(queue, 8);
    cinux::fs::ext2::Ext2     volume(cache, frozen_clock);
    ASSERT_TRUE(volume.mount().ok());
    ASSERT_TRUE(volume.make_dir("gone").ok());
    const auto kHeld = volume.lookup_held("gone");
    ASSERT_TRUE(kHeld.ok());
    ASSERT_TRUE(volume.unlink("gone").ok());
    const auto kChild = volume.make_file_at(kHeld.value(), "orphan");
    volume.release(kHeld.value());
    ASSERT_EQ(kChild.error(), cinux::base::KernelError::kNotFound);
    ASSERT_TRUE(volume.sync());
    ASSERT_TRUE(fsck_reports_clean("/tmp/cinux-review-detached.img", disk.image()));
}
