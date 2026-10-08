#include <string>
#include <string_view>

#include "../framework/framework.hpp"
#include "cinux/memory.hpp"
#include "cinux/result.hpp"
#include "kernel/fs/file_table.hpp"
#include "kernel/fs/fs_config.hpp"
#include "kernel/fs/ramfs/ramfs.hpp"
#include "kernel/fs/vfs.hpp"
#include "test_assert.hpp"

namespace {

using cinux::fs::FsDirent;
using cinux::fs::InodeType;
using cinux::fs::RamFs;

}  // namespace

TEST("ramfs: the root is born as directory one") {
    RamFs      ram;
    const auto kRoot = ram.lookup("");
    ASSERT_TRUE(kRoot.ok());
    ASSERT_TRUE(kRoot.value().type == InodeType::kDirectory);
    ASSERT_EQ(kRoot.value().ino, 1UL);
}

TEST("ramfs: a created file is found again with fresh metadata") {
    RamFs ram;
    ASSERT_TRUE(ram.make_file("a.txt").ok());
    const auto kFound = ram.lookup("a.txt");
    ASSERT_TRUE(kFound.ok());
    ASSERT_TRUE(kFound.value().type == InodeType::kFile);
    ASSERT_EQ(kFound.value().size, 0UL);

    const auto kMissing = ram.lookup("b.txt");
    ASSERT_FALSE(kMissing.ok());
    ASSERT_EQ(kMissing.error(), cinux::base::KernelError::kNotFound);
}

TEST("ramfs: writes land and read back, size follows the tail") {
    RamFs ram;
    ASSERT_TRUE(ram.make_file("note").ok());
    const auto kNode = ram.lookup("note");

    const std::string kText = "hello, ramfs";
    const auto        kDone = ram.write(kNode.value(), 0, kText.data(), kText.size());
    ASSERT_TRUE(kDone.ok());
    ASSERT_EQ(kDone.value(), kText.size());

    char       back[16] = {};
    const auto kRead    = ram.read(kNode.value(), 0, back, sizeof(back));
    ASSERT_EQ(kRead.value(), kText.size());
    ASSERT_TRUE(cinux::base::EqualBytes(back, kText.data(), kText.size()));

    const auto kAfter = ram.lookup("note");
    ASSERT_EQ(kAfter.value().size, kText.size());
}

TEST("ramfs: reads clamp at the end of the file") {
    RamFs ram;
    ASSERT_TRUE(ram.make_file("tiny").ok());
    const auto        kNode = ram.lookup("tiny");
    const std::string kFour(4, 'x');
    ASSERT_TRUE(ram.write(kNode.value(), 0, kFour.data(), 4).ok());

    char       back[8] = {};
    const auto kTail   = ram.read(kNode.value(), 2, back, 8);
    ASSERT_EQ(kTail.value(), 2UL);
    const auto kPast = ram.read(kNode.value(), 4, back, 8);
    ASSERT_EQ(kPast.value(), 0UL);
}

TEST("ramfs: a hole inside reallocated growth reads back as zeros") {
    RamFs ram;
    ASSERT_TRUE(ram.make_file("sparse").ok());
    const auto kNode = ram.lookup("sparse");

    const std::string kHead(50, 'A');
    const std::string kTail(50, 'B');
    ASSERT_TRUE(ram.write(kNode.value(), 0, kHead.data(), 50).ok());
    ASSERT_TRUE(ram.write(kNode.value(), 4090, kTail.data(), 50).ok());

    const auto kAgain = ram.lookup("sparse");
    ASSERT_EQ(kAgain.value().size, 4140UL);

    static constexpr unsigned long kGapBytes      = 4090 - 50;
    char                           gap[kGapBytes] = {};
    const auto                     kGapRead       = ram.read(kNode.value(), 50, gap, kGapBytes);
    ASSERT_EQ(kGapRead.value(), kGapBytes);
    ASSERT_TRUE(cinux::base::BytesAre(gap, 0, kGapBytes));
}

TEST("ramfs: a hole inside spare capacity is zeroed too") {
    RamFs ram;
    ASSERT_TRUE(ram.make_file("spare").ok());
    const auto kNode = ram.lookup("spare");

    const std::string kHead(100, 'a');
    const std::string kTail(50, 'b');
    ASSERT_TRUE(ram.write(kNode.value(), 0, kHead.data(), 100).ok());
    ASSERT_TRUE(ram.write(kNode.value(), 4000, kTail.data(), 50).ok());

    static constexpr unsigned long kGapBytes      = 4000 - 100;
    char                           gap[kGapBytes] = {};
    const auto                     kGapRead       = ram.read(kNode.value(), 100, gap, kGapBytes);
    ASSERT_EQ(kGapRead.value(), kGapBytes);
    ASSERT_TRUE(cinux::base::BytesAre(gap, 0, kGapBytes));
}

TEST("ramfs: directories nest and files live inside them") {
    RamFs ram;
    ASSERT_TRUE(ram.make_dir("etc").ok());
    ASSERT_TRUE(ram.make_file("etc/motd").ok());
    ASSERT_TRUE(ram.make_dir("etc/extra").ok());

    const auto kFile = ram.lookup("etc/motd");
    ASSERT_TRUE(kFile.ok());
    ASSERT_TRUE(kFile.value().type == InodeType::kFile);

    const auto kDir = ram.lookup("etc/extra");
    ASSERT_TRUE(kDir.ok());
    ASSERT_TRUE(kDir.value().type == InodeType::kDirectory);
}

TEST("ramfs: a path through a file stops with kNotADirectory") {
    RamFs ram;
    ASSERT_TRUE(ram.make_file("plain").ok());
    const auto kThrough = ram.lookup("plain/deeper");
    ASSERT_FALSE(kThrough.ok());
    ASSERT_EQ(kThrough.error(), cinux::base::KernelError::kNotADirectory);
}

TEST("ramfs: creation refuses duplicates, slashes and over-long names") {
    RamFs ram;
    ASSERT_TRUE(ram.make_file("same").ok());
    const auto kDuplicate = ram.make_file("same");
    ASSERT_FALSE(kDuplicate.ok());
    ASSERT_EQ(kDuplicate.error(), cinux::base::KernelError::kAlreadyExists);

    const auto kMissingDir = ram.make_file("ghost/a");
    ASSERT_EQ(kMissingDir.error(), cinux::base::KernelError::kNotFound);
    const auto kEmpty = ram.make_file("");
    ASSERT_EQ(kEmpty.error(), cinux::base::KernelError::kInvalidArgument);

    const std::string kLongName(cinux::fs::kFsNameMax, 'n');
    const auto        kOverLong = ram.make_file(kLongName);
    ASSERT_EQ(kOverLong.error(), cinux::base::KernelError::kInvalidArgument);
}

TEST("ramfs: unlink refuses a directory that still holds entries") {
    RamFs ram;
    ASSERT_TRUE(ram.make_dir("bag").ok());
    ASSERT_TRUE(ram.make_file("bag/stone").ok());

    const auto kFull = ram.unlink("bag");
    ASSERT_FALSE(kFull.ok());
    ASSERT_EQ(kFull.error(), cinux::base::KernelError::kDirectoryNotEmpty);
    const auto kStillHere = ram.lookup("bag/stone");
    ASSERT_TRUE(kStillHere.ok());
}

TEST("ramfs: unlink removes files, then the emptied directory") {
    RamFs ram;
    ASSERT_TRUE(ram.make_dir("bag").ok());
    ASSERT_TRUE(ram.make_file("bag/stone").ok());

    ASSERT_TRUE(ram.unlink("bag/stone").ok());
    ASSERT_FALSE(ram.lookup("bag/stone").ok());
    ASSERT_TRUE(ram.unlink("bag").ok());
    ASSERT_FALSE(ram.lookup("bag").ok());
}

TEST("ramfs: unlink reports missing names and trailing slashes") {
    RamFs      ram;
    const auto kGhost = ram.unlink("bag");
    ASSERT_EQ(kGhost.error(), cinux::base::KernelError::kNotFound);
    const auto kSlash = ram.unlink("bag/");
    ASSERT_EQ(kSlash.error(), cinux::base::KernelError::kInvalidArgument);
}

TEST("ramfs: read_dir hands the newest children out first") {
    RamFs ram;
    ASSERT_TRUE(ram.make_file("b").ok());
    ASSERT_TRUE(ram.make_file("a").ok());
    ASSERT_TRUE(ram.make_file("c").ok());
    const auto kRoot = ram.lookup("");

    FsDirent entry{};
    ASSERT_TRUE(ram.read_dir(kRoot.value(), 0, entry).value());
    ASSERT_EQ(std::string_view(entry.name), std::string_view("c"));
    ASSERT_TRUE(ram.read_dir(kRoot.value(), 1, entry).value());
    ASSERT_EQ(std::string_view(entry.name), std::string_view("a"));
}

TEST("ramfs: read_dir reaches the oldest child last") {
    RamFs ram;
    ASSERT_TRUE(ram.make_file("b").ok());
    ASSERT_TRUE(ram.make_file("a").ok());
    ASSERT_TRUE(ram.make_file("c").ok());
    const auto kRoot = ram.lookup("");

    FsDirent entry{};
    ASSERT_TRUE(ram.read_dir(kRoot.value(), 2, entry).value());
    ASSERT_EQ(std::string_view(entry.name), std::string_view("b"));
}

TEST("ramfs: read_dir ends with false past the last child") {
    RamFs ram;
    ASSERT_TRUE(ram.make_file("only").ok());
    const auto kRoot = ram.lookup("");

    FsDirent   entry{};
    const auto kPast = ram.read_dir(kRoot.value(), 1, entry);
    ASSERT_TRUE(kPast.ok());
    ASSERT_FALSE(kPast.value());
}

TEST("ramfs: files and directories refuse each other's verbs") {
    RamFs ram;
    ASSERT_TRUE(ram.make_dir("d").ok());
    ASSERT_TRUE(ram.make_file("f").ok());
    const auto kDir  = ram.lookup("d");
    const auto kFile = ram.lookup("f");

    const std::string kText     = "x";
    const auto        kWriteDir = ram.write(kDir.value(), 0, kText.data(), 1);
    ASSERT_EQ(kWriteDir.error(), cinux::base::KernelError::kIsADirectory);
    char       sink[1]  = {};
    const auto kReadDir = ram.read(kDir.value(), 0, sink, 1);
    ASSERT_EQ(kReadDir.error(), cinux::base::KernelError::kIsADirectory);

    FsDirent   entry{};
    const auto kListFile = ram.read_dir(kFile.value(), 0, entry);
    ASSERT_EQ(kListFile.error(), cinux::base::KernelError::kNotADirectory);
}

TEST("ramfs: the whole floor works through the mount table's erased face") {
    cinux::fs::Vfs vfs;
    RamFs          ram;
    ASSERT_TRUE(vfs.mount("/", ram).ok());
    const auto kRoot = vfs.resolve("/");
    ASSERT_TRUE(kRoot.ok());

    ASSERT_TRUE(kRoot.value().ops->make_dir(kRoot.value().self, "etc").ok());
    const auto kMade = kRoot.value().ops->make_file(kRoot.value().self, "etc/motd");
    ASSERT_TRUE(kMade.ok());

    const std::string kText = "welcome";
    const auto        kWritten =
        kRoot.value().ops->write(kRoot.value().self, kMade.value(), 0, kText.data(), kText.size());
    ASSERT_EQ(kWritten.value(), kText.size());

    char       back[16] = {};
    const auto kRead =
        kRoot.value().ops->read(kRoot.value().self, kMade.value(), 0, back, sizeof(back));
    ASSERT_EQ(kRead.value(), kText.size());
    ASSERT_TRUE(cinux::base::EqualBytes(back, kText.data(), kText.size()));
}

TEST("ramfs: an unlinked file stays readable through its open descriptor") {
    RamFs                ram;
    const auto           kOps = cinux::fs::EraseFs<RamFs>();
    cinux::fs::FileTable table;
    const auto           kNode = ram.make_file("held").value();
    ASSERT_TRUE(ram.write(kNode, 0, "old", 3).ok());
    const auto kFd = table.alloc(&kOps, &ram, kNode).value();
    ASSERT_TRUE(ram.unlink("held").ok());
    ASSERT_FALSE(ram.lookup("held").ok());
    auto* const kSlot = table.get(kFd).value();
    char        bytes[4]{};
    ASSERT_TRUE(kSlot->ops->read(kSlot->backend, kSlot->node, 0, bytes, 3).value() == 3UL);
    ASSERT_STREQ(bytes, "old");
    ASSERT_TRUE(table.free(kFd).ok());
}

TEST("ramfs: an unlinked file stays writable through its open descriptor") {
    RamFs                ram;
    const auto           kOps = cinux::fs::EraseFs<RamFs>();
    cinux::fs::FileTable table;
    const auto           kNode = ram.make_file("held").value();
    const auto           kFd   = table.alloc(&kOps, &ram, kNode).value();
    ASSERT_TRUE(ram.unlink("held").ok());
    auto* const kSlot = table.get(kFd).value();
    ASSERT_TRUE(kSlot->ops->write(kSlot->backend, kSlot->node, 0, "new", 3).value() == 3UL);
    char bytes[4]{};
    ASSERT_TRUE(kSlot->ops->read(kSlot->backend, kSlot->node, 0, bytes, 3).value() == 3UL);
    ASSERT_STREQ(bytes, "new");
}

TEST("ramfs: closing one descriptor keeps another reference to an unlinked file alive") {
    RamFs                ram;
    const auto           kOps = cinux::fs::EraseFs<RamFs>();
    cinux::fs::FileTable table;
    const auto           kNode = ram.make_file("twice").value();
    ASSERT_TRUE(ram.write(kNode, 0, "two", 3).ok());
    const auto kFirst  = table.alloc(&kOps, &ram, kNode).value();
    const auto kSecond = table.alloc(&kOps, &ram, kNode).value();
    ASSERT_TRUE(ram.unlink("twice").ok());
    ASSERT_TRUE(table.free(kFirst).ok());
    auto* const kSlot    = table.get(kSecond).value();
    char        bytes[4] = {};
    ASSERT_TRUE(kSlot->ops->read(kSlot->backend, kSlot->node, 0, bytes, 3).value() == 3UL);
    ASSERT_STREQ(bytes, "two");
    ASSERT_TRUE(table.free(kSecond).ok());
}

TEST("ramfs: a reused name identifies a new node while the old descriptor stays open") {
    RamFs                ram;
    const auto           kOps = cinux::fs::EraseFs<RamFs>();
    cinux::fs::FileTable table;
    const auto           kOld = ram.make_file("same").value();
    const auto           kFd  = table.alloc(&kOps, &ram, kOld).value();
    ASSERT_TRUE(ram.write(kOld, 0, "old", 3).ok());
    ASSERT_TRUE(ram.unlink("same").ok());
    const auto kNew = ram.make_file("same").value();
    ASSERT_NE(kOld.ino, kNew.ino);
    ASSERT_TRUE(ram.write(kNew, 0, "new", 3).ok());
    char bytes[4] = {};
    ASSERT_TRUE(ram.read(table.get(kFd).value()->node, 0, bytes, 3).value() == 3UL);
    ASSERT_STREQ(bytes, "old");
    ASSERT_TRUE(ram.read(kNew, 0, bytes, 3).value() == 3UL);
    ASSERT_STREQ(bytes, "new");
}

TEST("ramfs: an open empty directory survives removal from its parent") {
    RamFs                ram;
    const auto           kOps = cinux::fs::EraseFs<RamFs>();
    cinux::fs::FileTable table;
    const auto           kDir = ram.make_dir("gone").value();
    const auto           kFd  = table.alloc(&kOps, &ram, kDir).value();
    ASSERT_TRUE(ram.unlink("gone").ok());
    ASSERT_FALSE(ram.lookup("gone").ok());
    cinux::fs::FsDirent entry{};
    ASSERT_FALSE(ram.read_dir(table.get(kFd).value()->node, 0, entry).value());
}

TEST("ramfs: closing a linked file leaves its directory entry usable") {
    RamFs                ram;
    const auto           kOps = cinux::fs::EraseFs<RamFs>();
    cinux::fs::FileTable table;
    const auto           kNode = ram.make_file("linked").value();
    const auto           kFd   = table.alloc(&kOps, &ram, kNode).value();
    ASSERT_TRUE(table.free(kFd).ok());
    ASSERT_TRUE(ram.lookup("linked").value().ino == kNode.ino);
    ASSERT_TRUE(ram.unlink("linked").ok());
}

int main() {
    return cinux::test::RunAll();
}
