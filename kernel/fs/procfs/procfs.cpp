#include "kernel/fs/procfs/procfs.hpp"

#include <string_view>

#include "cinux/memory.hpp"
#include "cinux/result.hpp"
#include "kernel/fs/fs_config.hpp"
#include "kernel/fs/vfs.hpp"
#include "kernel/proc/task_snapshot.hpp"
#include "kernel/syscall/syscall.hpp"

namespace cinux::fs {

namespace {

/// The root's inode.
constexpr unsigned long kRootIno = 1;

/// The meminfo file's inode.
constexpr unsigned long kMeminfoIno = 2;

/// The self directory's inode.
constexpr unsigned long kSelfIno = 3;

/// One pid directory's stride in the inode space.
constexpr unsigned long kPidStride = 4;

/// One process directory's entries, stat first, cmdline second.
constexpr unsigned long kPidEntries = 2;

/// Writes one decimal word; returns the bytes written.
unsigned long put_decimal(char* out, unsigned long value) {
    char          digits[24] = {};
    unsigned long count      = 0;
    if (value == 0) {
        out[0] = '0';
        return 1;
    }
    while (value != 0) {
        digits[count] = static_cast<char>('0' + (value % 10UL));
        ++count;
        value /= 10UL;
    }
    for (unsigned long index = 0; index < count; ++index) {
        out[index] = digits[count - 1 - index];
    }
    return count;
}

unsigned long put_text(char* out, std::string_view text) {
    for (unsigned long index = 0; index < text.size(); ++index) {
        out[index] = text[index];
    }
    return text.size();
}

/// The Linux stat layout through field 24 (rss); unavailable accounting
/// fields are zero, with no terminal represented by tty 0 and tpgid -1.
unsigned long format_stat(char* out, unsigned long out_cap, const proc::TaskSnapshot& shot) {
    unsigned long used = 0;
    used += put_decimal(out + used, static_cast<unsigned long>(shot.pid));
    out[used] = ' ';
    ++used;
    out[used] = '(';
    ++used;
    for (unsigned long index = 0; shot.name[index] != '\0' && index < proc::kTaskNameMax; ++index) {
        out[used] = shot.name[index];
        ++used;
    }
    out[used] = ')';
    ++used;
    out[used] = ' ';
    ++used;
    out[used] = shot.state;
    ++used;
    out[used] = ' ';
    ++used;
    used += put_decimal(out + used, static_cast<unsigned long>(shot.parent_pid));
    out[used] = ' ';
    ++used;
    used += put_decimal(out + used, static_cast<unsigned long>(shot.pgid));
    out[used] = ' ';
    ++used;
    used += put_decimal(out + used, static_cast<unsigned long>(shot.sid));
    used += put_text(out + used, " 0 -1");
    for (unsigned long field = 9; field <= 24; ++field) {
        used += put_text(out + used, " 0");
    }
    out[used] = '\n';
    ++used;
    return used < out_cap ? used : out_cap;
}

/// "MemTotal:\nMemFree:\n" in kilobytes, pages times four.
unsigned long format_meminfo(char* out, unsigned long out_cap, const ProcSource& source) {
    unsigned long free_pages  = 0;
    unsigned long total_pages = 0;
    if (source.memory != nullptr) {
        source.memory(source.context, free_pages, total_pages);
    }
    unsigned long used = 0;
    used += put_text(out + used, "MemTotal:  ");
    used += put_decimal(out + used, total_pages * 4UL);
    used += put_text(out + used, " kB\nMemFree:   ");
    used += put_decimal(out + used, free_pages * 4UL);
    used += put_text(out + used, " kB\n");
    return used < out_cap ? used : out_cap;
}

/// The name bytes plus one NUL, the whole cmdline this kernel keeps.
unsigned long format_cmdline(char* out, unsigned long out_cap, const proc::TaskSnapshot& shot) {
    unsigned long used = 0;
    for (unsigned long index = 0; shot.name[index] != '\0' && index < proc::kTaskNameMax; ++index) {
        out[used] = shot.name[index];
        ++used;
    }
    out[used] = '\0';
    ++used;
    return used < out_cap ? used : out_cap;
}

/// Parses a whole decimal word; false on any other shape.
bool parse_whole_decimal(std::string_view text, unsigned long& value) {
    if (text.empty() || text.size() > 9) {
        return false;
    }
    unsigned long parsed = 0;
    for (const char kDigit : text) {
        if (kDigit < '0' || kDigit > '9') {
            return false;
        }
        parsed = (parsed * 10) + static_cast<unsigned long>(kDigit - '0');
    }
    value = parsed;
    return true;
}

/// Whether a pid is alive through this source.
bool tgid_lives(const ProcSource& source, unsigned long tgid) {
    proc::TaskSnapshot shot{};
    return source.snapshot != nullptr && source.snapshot(source.context, tgid, shot);
}

/// Generates one process file's bytes for a living task.
// NOLINTNEXTLINE(readability-function-size) the scratch ladder names five
bool generate_process(const ProcSource& source, unsigned long tgid, unsigned long kind,
                      char* scratch, unsigned long scratch_cap, unsigned long& bytes) {
    proc::TaskSnapshot shot{};
    if (source.snapshot == nullptr || !source.snapshot(source.context, tgid, shot)) {
        bytes = 0;
        return true;
    }
    if (kind == 1) {
        bytes = format_stat(scratch, scratch_cap, shot);
        return true;
    }
    bytes = format_cmdline(scratch, scratch_cap, shot);
    return true;
}

/// Generates one pseudo file's bytes; false for an unknown inode.
// NOLINTNEXTLINE(readability-function-size) the scratch ladder names five
bool generate(const ProcSource& source, unsigned long ino, char* scratch, unsigned long scratch_cap,
              unsigned long& bytes) {
    if (ino == kMeminfoIno) {
        bytes = format_meminfo(scratch, scratch_cap, source);
        return true;
    }
    if (ino < kProcPidBase) {
        return false;
    }
    const unsigned long kSeat = ino - kProcPidBase;
    const unsigned long kTgid = kSeat / kPidStride;
    const unsigned long kKind = kSeat % kPidStride;
    if (kKind == 0 || kKind > kPidEntries) {
        return false;
    }
    return generate_process(source, kTgid, kKind, scratch, scratch_cap, bytes);
}

}  // namespace

void ProcFs::set_source(const ProcSource& source) {
    source_ = source;
}

base::Result<FsNode> ProcFs::lookup(std::string_view path) {
    if (path.empty()) {
        return FsNode{.opaque = nullptr, .type = InodeType::kDirectory, .ino = kRootIno, .size = 0};
    }
    if (path == "meminfo") {
        return FsNode{.opaque = nullptr, .type = InodeType::kFile, .ino = kMeminfoIno, .size = 0};
    }
    if (path == "self") {
        return FsNode{.opaque = nullptr, .type = InodeType::kDirectory, .ino = kSelfIno, .size = 0};
    }
    if (path == "self/exe") {
        return FsNode{.opaque = nullptr, .type = InodeType::kFile, .ino = kProcExeIno, .size = 0};
    }
    const unsigned long kSlash = path.find('/');
    if (kSlash == std::string_view::npos) {
        unsigned long tgid = 0;
        if (!parse_whole_decimal(path, tgid) || !tgid_lives(source_, tgid)) {
            return base::KernelError::kNotFound;
        }
        const unsigned long kIno = kProcPidBase + (tgid * kPidStride);
        return FsNode{.opaque = nullptr, .type = InodeType::kDirectory, .ino = kIno, .size = 0};
    }
    unsigned long tgid = 0;
    if (!parse_whole_decimal(path.substr(0, kSlash), tgid) || !tgid_lives(source_, tgid)) {
        return base::KernelError::kNotFound;
    }
    const std::string_view kLeaf = path.substr(kSlash + 1);
    if (kLeaf == "stat") {
        const unsigned long kIno = kProcPidBase + (tgid * kPidStride) + 1;
        return FsNode{.opaque = nullptr, .type = InodeType::kFile, .ino = kIno, .size = 0};
    }
    if (kLeaf == "cmdline") {
        const unsigned long kIno = kProcPidBase + (tgid * kPidStride) + 2;
        return FsNode{.opaque = nullptr, .type = InodeType::kFile, .ino = kIno, .size = 0};
    }
    return base::KernelError::kNotFound;
}

base::Result<FsNode> ProcFs::make_file(std::string_view /*path*/) {
    return base::KernelError::kNotSupported;
}

base::Result<FsNode> ProcFs::make_dir(std::string_view /*path*/) {
    return base::KernelError::kNotSupported;
}

base::Result<void> ProcFs::unlink(std::string_view /*path*/) {
    return base::KernelError::kNotSupported;
}

base::Result<unsigned long> ProcFs::read(FsNode node, unsigned long offset, void* sink,
                                         unsigned long count) {
    if (node.type != InodeType::kFile) {
        return base::KernelError::kIsADirectory;
    }
    if (node.ino == kProcExeIno) {
        if (source_.exe_bytes == nullptr) {
            return 0;
        }
        unsigned long gave = 0;
        if (!source_.exe_bytes(source_.context, offset, sink, count, gave)) {
            return 0;
        }
        return gave;
    }
    char          scratch[192] = {};
    unsigned long bytes        = 0;
    if (!generate(source_, node.ino, scratch, sizeof(scratch), bytes)) {
        return base::KernelError::kNotFound;
    }
    if (offset >= bytes) {
        return 0;
    }
    const unsigned long kHave = bytes - offset;
    const unsigned long kGive = kHave < count ? kHave : count;
    base::CopyBytes(sink, scratch + offset, kGive);
    return kGive;
}

base::Result<unsigned long> ProcFs::write(FsNode /*node*/, unsigned long /*offset*/,
                                          const void* /*source*/, unsigned long /*count*/) {
    return base::KernelError::kNotSupported;
}

base::Result<bool> ProcFs::read_dir(FsNode node, unsigned long index, FsDirent& entry) const {
    if (node.type != InodeType::kDirectory) {
        return base::KernelError::kNotADirectory;
    }
    entry      = FsDirent{};
    entry.type = InodeType::kDirectory;
    if (node.ino == kRootIno) {
        if (index == 0) {
            entry.name[0] = '.';
            entry.ino     = kRootIno;
            return true;
        }
        if (index == 1) {
            entry.name[0] = '.';
            entry.name[1] = '.';
            entry.ino     = kRootIno;
            return true;
        }
        if (index == 2) {
            base::CopyBytes(entry.name, "meminfo", 8);
            entry.ino  = kMeminfoIno;
            entry.type = InodeType::kFile;
            return true;
        }
        if (index == 3) {
            base::CopyBytes(entry.name, "self", 5);
            entry.ino = kSelfIno;
            return true;
        }
        unsigned long tgid = 0;
        if (source_.nth_tgid == nullptr || !source_.nth_tgid(source_.context, index - 4, tgid)) {
            return false;
        }
        const unsigned long kNameLen = put_decimal(entry.name, tgid);
        entry.name[kNameLen]         = '\0';
        entry.ino                    = kProcPidBase + (tgid * kPidStride);
        return true;
    }
    if (index == 0) {
        entry.name[0] = '.';
        entry.ino     = node.ino;
        return true;
    }
    if (index == 1) {
        entry.name[0] = '.';
        entry.name[1] = '.';
        entry.ino     = kRootIno;
        return true;
    }
    if (index == 2) {
        base::CopyBytes(entry.name, "stat", 5);
        entry.ino  = node.ino + 1;
        entry.type = InodeType::kFile;
        return true;
    }
    if (index == 3) {
        base::CopyBytes(entry.name, "cmdline", 8);
        entry.ino  = node.ino + 2;
        entry.type = InodeType::kFile;
        return true;
    }
    return false;
}

base::Result<void> ProcFs::stat(FsNode node, FsStat& probed) {
    probed       = FsStat{};
    probed.ino   = node.ino;
    probed.links = 1;
    if (node.type == InodeType::kDirectory) {
        probed.mode = kFsDirMode;
        return {};
    }
    probed.mode = kFsFileMode;
    if (node.ino == kProcExeIno) {
        probed.size = source_.exe_size != nullptr ? source_.exe_size(source_.context) : 0;
        return {};
    }
    char          scratch[192] = {};
    unsigned long bytes        = 0;
    if (generate(source_, node.ino, scratch, sizeof(scratch), bytes)) {
        probed.size = bytes;
    }
    return {};
}

void ProcFs::retain(FsNode /*node*/) {}

void ProcFs::release(FsNode /*node*/) {}

bool ProcFs::readable(FsNode /*node*/) {
    return true;
}

long long ProcFs::ioctl(FsNode /*node*/, unsigned long long /*word*/,
                        unsigned long long /*argument*/) {
    return -syscall::kEnotty;
}

base::Result<FsNode> ProcFs::adopt_open(FsNode node, std::string_view /*path*/) {
    return node;
}

}  // namespace cinux::fs
