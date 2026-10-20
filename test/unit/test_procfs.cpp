#include <cstring>

#include "../framework/framework.hpp"
#include "cinux/result.hpp"
#include "kernel/fs/procfs/procfs.hpp"
#include "kernel/fs/vfs.hpp"
#include "kernel/proc/task_snapshot.hpp"
#include "test_assert.hpp"

namespace {

unsigned long g_tgids[4] = {1, 2, 7, 12};

bool fake_nth_tgid(void* /*context*/, unsigned long ordinal, unsigned long& tgid) {
    if (ordinal >= 4) {
        return false;
    }
    tgid = g_tgids[ordinal];
    return true;
}

bool fake_snapshot(void* /*context*/, unsigned long tgid, cinux::proc::TaskSnapshot& into) {
    if (tgid != 2 && tgid != 7) {
        return false;
    }
    into.pid                = static_cast<long>(tgid);
    into.parent_pid         = 1;
    into.pgid               = static_cast<long>(tgid);
    into.sid                = 2;
    into.state              = 'S';
    const char* const kName = tgid == 2 ? "sh" : "ttydemo";
    for (unsigned long index = 0; kName[index] != '\0'; ++index) {
        into.name[index] = kName[index];
    }
    return true;
}

unsigned long fake_exe_path(void* /*context*/, char* out, unsigned long cap) {
    const char* const kPath = "/bin/sh";
    unsigned long     used  = 0;
    while (used + 1 < cap && kPath[used] != '\0') {
        out[used] = kPath[used];
        ++used;
    }
    out[used] = '\0';
    return used;
}

unsigned char g_exe_image[8] = {0xDE, 0xAD, 0xBE, 0xEF, 1, 2, 3, 4};

// NOLINTNEXTLINE(readability-function-size) the source seat names five
bool fake_exe_bytes(void* /*context*/, unsigned long offset, void* sink, unsigned long count,
                    unsigned long& gave) {
    if (offset >= sizeof(g_exe_image)) {
        gave = 0;
        return true;
    }
    gave = count < (sizeof(g_exe_image) - offset) ? count : (sizeof(g_exe_image) - offset);
    for (unsigned long index = 0; index < gave; ++index) {
        static_cast<unsigned char*>(sink)[index] = g_exe_image[offset + index];
    }
    return true;
}

unsigned long fake_exe_size(void* /*context*/) {
    return sizeof(g_exe_image);
}

void fake_memory(void* /*context*/, unsigned long& free_pages, unsigned long& total_pages) {
    free_pages  = 100;
    total_pages = 200;
}

const cinux::fs::ProcSource kFakeSource{
    .nth_tgid  = fake_nth_tgid,
    .snapshot  = fake_snapshot,
    .exe_path  = fake_exe_path,
    .exe_bytes = fake_exe_bytes,
    .exe_size  = fake_exe_size,
    .memory    = fake_memory,
    .context   = nullptr,
};

/// One procfs wired to the fake facts, the shape every case mounts.
cinux::fs::ProcFs& stocked(cinux::fs::ProcFs& procfs) {
    procfs.set_source(kFakeSource);
    return procfs;
}

bool text_at(cinux::fs::ProcFs& procfs, unsigned long ino, char* out, unsigned long cap) {
    const cinux::base::Result<cinux::fs::FsNode> kNode =
        procfs.lookup(ino == cinux::fs::kProcExeIno ? "self/exe" : "meminfo");
    if (!kNode.ok()) {
        return false;
    }
    const cinux::base::Result<unsigned long> kGot = procfs.read(kNode.value(), 0, out, cap - 1);
    if (!kGot.ok()) {
        return false;
    }
    out[kGot.value()] = '\0';
    return true;
}

}  // namespace

TEST("procfs: the root lists meminfo and self first") {
    cinux::fs::ProcFs                            stock;
    const cinux::base::Result<cinux::fs::FsNode> kRoot = stocked(stock).lookup("");

    cinux::fs::FsDirent entry;
    ASSERT_TRUE(stock.read_dir(kRoot.value(), 2, entry).value());
    ASSERT_STREQ(entry.name, "meminfo");
    ASSERT_TRUE(stock.read_dir(kRoot.value(), 3, entry).value());
    ASSERT_STREQ(entry.name, "self");
}

TEST("procfs: the root then lists living pids in walk order") {
    cinux::fs::ProcFs                            stock;
    const cinux::base::Result<cinux::fs::FsNode> kRoot = stocked(stock).lookup("");

    cinux::fs::FsDirent entry;
    ASSERT_TRUE(stock.read_dir(kRoot.value(), 4, entry).value());
    ASSERT_STREQ(entry.name, "1");
    ASSERT_TRUE(stock.read_dir(kRoot.value(), 5, entry).value());
    ASSERT_STREQ(entry.name, "2");
}

TEST("procfs: the pid walk ends past the last leader") {
    cinux::fs::ProcFs                            stock;
    const cinux::base::Result<cinux::fs::FsNode> kRoot = stocked(stock).lookup("");

    cinux::fs::FsDirent entry;
    ASSERT_TRUE(stock.read_dir(kRoot.value(), 7, entry).value());
    ASSERT_STREQ(entry.name, "12");
    ASSERT_FALSE(stock.read_dir(kRoot.value(), 8, entry).value());
}

TEST("procfs: meminfo answers both counters in kilobytes") {
    cinux::fs::ProcFs stock;
    stocked(stock);
    char text[96] = {};
    ASSERT_TRUE(text_at(stock, 2, text, sizeof(text)));
    ASSERT_TRUE(std::strstr(text, "MemTotal:  800 kB") != nullptr);
    ASSERT_TRUE(std::strstr(text, "MemFree:   400 kB") != nullptr);
}

TEST("procfs: a living pid directory serves stat then cmdline") {
    cinux::fs::ProcFs stock;
    stocked(stock);
    const cinux::base::Result<cinux::fs::FsNode> kDir = stock.lookup("7");
    ASSERT_TRUE(kDir.ok());
    ASSERT_EQ(kDir.value().type, cinux::fs::InodeType::kDirectory);

    cinux::fs::FsDirent entry;
    ASSERT_TRUE(stock.read_dir(kDir.value(), 2, entry).value());
    ASSERT_STREQ(entry.name, "stat");
}

TEST("procfs: the pid directory ends after cmdline") {
    cinux::fs::ProcFs stock;
    stocked(stock);
    const cinux::base::Result<cinux::fs::FsNode> kDir = stock.lookup("7");
    ASSERT_TRUE(kDir.ok());

    cinux::fs::FsDirent entry;
    ASSERT_TRUE(stock.read_dir(kDir.value(), 3, entry).value());
    ASSERT_STREQ(entry.name, "cmdline");
    ASSERT_FALSE(stock.read_dir(kDir.value(), 4, entry).value());
}

TEST("procfs: stat spells the snapshot's leading fields") {
    cinux::fs::ProcFs stock;
    stocked(stock);
    const cinux::base::Result<cinux::fs::FsNode> kStat = stock.lookup("7/stat");
    ASSERT_TRUE(kStat.ok());
    char                                     text[96] = {};
    const cinux::base::Result<unsigned long> kGot =
        stock.read(kStat.value(), 0, text, sizeof(text) - 1);
    ASSERT_TRUE(kGot.ok());
    text[kGot.value()] = '\0';
    ASSERT_STREQ(text, "7 (ttydemo) S 1 7 2 0 -1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n");
}

TEST("procfs: a dead pid answers NotFound") {
    cinux::fs::ProcFs stock;
    stocked(stock);
    ASSERT_EQ(stock.lookup("9").error(), cinux::base::KernelError::kNotFound);
    ASSERT_EQ(stock.lookup("9/stat").error(), cinux::base::KernelError::kNotFound);
}

TEST("procfs: self/exe hands over the delegate's bytes") {
    cinux::fs::ProcFs stock;
    stocked(stock);
    const cinux::base::Result<cinux::fs::FsNode> kExe = stock.lookup("self/exe");
    ASSERT_TRUE(kExe.ok());
    unsigned char       bytes[4] = {};
    const unsigned long kGot     = stock.read(kExe.value(), 4, bytes, sizeof(bytes)).value();
    ASSERT_EQ(kGot, 4UL);
    ASSERT_EQ(static_cast<unsigned>(bytes[0]), 1U);
    ASSERT_EQ(static_cast<unsigned>(bytes[3]), 4U);
}

TEST("procfs: stat answers the generated size for a pseudo file") {
    cinux::fs::ProcFs stock;
    stocked(stock);
    const cinux::base::Result<cinux::fs::FsNode> kStat = stock.lookup("2/stat");
    ASSERT_TRUE(kStat.ok());
    cinux::fs::FsStat probed{};
    ASSERT_TRUE(stock.stat(kStat.value(), probed).ok());
    ASSERT_EQ(probed.size, 52UL);
}

int main() {
    return cinux::test::RunAll();
}
