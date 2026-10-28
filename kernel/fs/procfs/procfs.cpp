#include "kernel/fs/procfs/procfs.hpp"

#include <iterator>
#include <span>
#include <string_view>

#include "cinux/container/name_table.hpp"
#include "cinux/format.hpp"
#include "cinux/literal_types.hpp"
#include "cinux/math.hpp"
#include "cinux/memory.hpp"
#include "cinux/parse.hpp"
#include "cinux/result.hpp"
#include "cinux/text.hpp"
#include "kernel/fs/devfs/devfs.hpp"
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

constexpr unsigned long kMountsIno  = 7;
constexpr unsigned long kUptimeIno  = 8;
constexpr unsigned long kCpuinfoIno = 9;

/// One pid directory's stride in the inode space.
constexpr unsigned long kPidStride = 4;

/// One process directory's entries, stat first, cmdline second.
constexpr unsigned long kPidEntries = 2;

/// One fixed directory name: what lookup answers and read_dir lists.
struct FixedName {
    const char*   name;
    InodeType     type;
    unsigned long step;
};

/// The root's fixed names, in listing order, dots aside.
constexpr FixedName kRootNames[] = {
    {.name = "meminfo", .type = InodeType::kFile, .step = kMeminfoIno - kRootIno},
    {.name = "self", .type = InodeType::kDirectory, .step = kSelfIno - kRootIno},
    {.name = "devices", .type = InodeType::kFile, .step = kProcDevicesIno - kRootIno},
    {.name = "mounts", .type = InodeType::kFile, .step = kMountsIno - kRootIno},
    {.name = "uptime", .type = InodeType::kFile, .step = kUptimeIno - kRootIno},
    {.name = "cpuinfo", .type = InodeType::kFile, .step = kCpuinfoIno - kRootIno},
};

/// One process directory's fixed names, stat first, cmdline second.
constexpr FixedName kPidDirNames[] = {
    {.name = "stat", .type = InodeType::kFile, .step = 1},
    {.name = "cmdline", .type = InodeType::kFile, .step = 2},
};

/// Serves one directory's fixed names, the index after the two dots.
template <unsigned long Count>
bool serve_fixed(const FixedName (&names)[Count], unsigned long base_ino, unsigned long index,
                 FsDirent& entry) {
    if (index < 2 || index - 2 >= Count) {
        return false;
    }
    const FixedName& fixed = names[index - 2];
    base::CopyText(entry.name, fixed.name);
    entry.ino  = base_ino + fixed.step;
    entry.type = fixed.type;
    return true;
}

/// The Linux stat layout through field 24 (rss); unavailable accounting
/// fields are zero, with no terminal represented by tty 0 and tpgid -1.
unsigned long format_stat(char* out, unsigned long out_cap, const proc::TaskSnapshot& shot) {
    const base::format::Arg kArgs[] = {
        base::format::NumS(shot.pid),
        base::format::Str(shot.name),
        base::format::NumU(static_cast<unsigned long long>(shot.state)),
        base::format::NumS(shot.parent_pid),
        base::format::NumS(shot.pgid),
        base::format::NumS(shot.sid),
    };
    base::format::FormatToBuf(out, out_cap,
                              "%ld (%s) %c %ld %ld %ld 0 -1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n",
                              base::format::ArgsView::of(kArgs));
    return base::StrLength(out, out_cap);
}

unsigned long format_meminfo(char* out, unsigned long out_cap, const ProcSource& source) {
    unsigned long free_pages  = 0;
    unsigned long total_pages = 0;
    if (source.memory != nullptr) {
        source.memory(source.context, free_pages, total_pages);
    }
    using base::operator""_KiB;
    const auto kTotalBytes = total_pages * source.memory_page_bytes;
    const auto kFreeBytes  = base::math::Min(free_pages, total_pages) * source.memory_page_bytes;
    const base::format::Arg kArgs[] = {
        base::format::NumU(kTotalBytes),        base::format::NumU(kTotalBytes - kFreeBytes),
        base::format::NumU(kFreeBytes),         base::format::NumU(kTotalBytes / 1_KiB),
        base::format::NumU(kFreeBytes / 1_KiB), base::format::NumU(kFreeBytes / 1_KiB),
    };
    base::format::FormatToBuf(out, out_cap,
                              "        total: used: free: shared: buffers: cached:\n"
                              "Mem: %lu %lu %lu 0 0 0\nSwap: 0 0 0\n"
                              "MemTotal:  %lu kB\nMemFree:   %lu kB\n"
                              "MemAvailable: %lu kB\nBuffers: 0 kB\nCached: 0 kB\n"
                              "SwapTotal: 0 kB\nSwapFree: 0 kB\n",
                              base::format::ArgsView::of(kArgs));
    return base::StrLength(out, out_cap);
}

unsigned long format_cpu(char* out, unsigned long capacity, const ProcCpuFacts& cpu) {
    const base::format::Arg kArgs[] = {
        base::format::NumU(cpu.processor), base::format::Str(cpu.vendor_id),
        base::format::NumU(cpu.family),    base::format::NumU(cpu.model),
        base::format::Str(cpu.model_name), base::format::NumU(cpu.stepping),
        base::format::NumU(cpu.apic_id),   base::format::Str(cpu.flags),
    };
    base::format::FormatToBuf(out, capacity,
                              "processor\t: %lu\nvendor_id\t: %s\ncpu family\t: %u\n"
                              "model\t\t: %u\nmodel name\t: %s\nstepping\t: %u\n"
                              "apicid\t\t: %u\nflags\t\t: %s\n\n",
                              base::format::ArgsView::of(kArgs));
    return base::StrLength(out, capacity);
}

unsigned long read_cpuinfo(const ProcSource& source, unsigned long offset, std::span<char> out) {
    if (source.nth_cpu == nullptr || out.empty()) {
        return 0;
    }
    unsigned long gave = 0;
    for (unsigned long ordinal = 0;; ++ordinal) {
        ProcCpuFacts cpu{};
        if (!source.nth_cpu(source.context, ordinal, cpu)) {
            break;
        }
        char       record[512]{};
        const auto kBytes = format_cpu(record, sizeof(record), cpu);
        if (offset >= kBytes) {
            offset -= kBytes;
            continue;
        }
        const auto kGive = base::math::Min(kBytes - offset, out.size() - gave);
        base::CopyBytes(out.data() + gave, record + offset, kGive);
        gave += kGive;
        offset = 0;
        if (gave == out.size()) {
            break;
        }
    }
    return gave;
}

unsigned long cpuinfo_size(const ProcSource& source) {
    if (source.nth_cpu == nullptr) {
        return 0;
    }
    unsigned long bytes = 0;
    for (unsigned long ordinal = 0;; ++ordinal) {
        ProcCpuFacts cpu{};
        if (!source.nth_cpu(source.context, ordinal, cpu)) {
            break;
        }
        char record[512]{};
        bytes += format_cpu(record, sizeof(record), cpu);
    }
    return bytes;
}

/// One class section of the devices node: the header, then every
/// seated device of that class in seating order.
unsigned long format_device_section(char* out, unsigned long out_cap, const ProcSource& source,
                                    const char* section) {
    const base::format::Arg kHeader[] = {
        base::format::Str(section),
    };
    base::format::FormatToBuf(out, out_cap, "%s:\n", base::format::ArgsView::of(kHeader));
    unsigned long used = base::StrLength(out, out_cap);
    if (source.nth_device == nullptr) {
        return used;
    }
    for (unsigned long ordinal = 0;; ++ordinal) {
        ProcDeviceFacts facts{};
        if (!source.nth_device(source.context, ordinal, facts)) {
            break;
        }
        if (std::string_view(facts.class_name) != std::string_view(section)) {
            continue;
        }
        if (out_cap - used < 2) {
            break;
        }
        if (facts.number != 0) {
            const DeviceNumber      kId     = SplitDeviceId(facts.number);
            const base::format::Arg kLine[] = {
                base::format::Str(facts.name),
                base::format::NumU(kId.major),
                base::format::NumU(kId.minor),
            };
            base::format::FormatToBuf(out + used, out_cap - used, "  %s %lu:%lu\n",
                                      base::format::ArgsView::of(kLine));
        } else {
            const base::format::Arg kLine[] = {
                base::format::Str(facts.name),
            };
            base::format::FormatToBuf(out + used, out_cap - used, "  %s\n",
                                      base::format::ArgsView::of(kLine));
        }
        used += base::StrLength(out + used, out_cap - used);
    }
    return used;
}

/// The devices node: three class sections, one line per device.
unsigned long format_devices(char* out, unsigned long out_cap, const ProcSource& source) {
    unsigned long used = 0;
    used += format_device_section(out + used, out_cap, source, kProcClassCharacter);
    if (used >= out_cap) {
        return out_cap;
    }
    used += format_device_section(out + used, out_cap - used, source, kProcClassBlock);
    if (used >= out_cap) {
        return out_cap;
    }
    used += format_device_section(out + used, out_cap - used, source, kProcClassNetwork);
    return used < out_cap ? used : out_cap;
}

/// The net/dev node: Linux's two header lines, then one row per
/// interface with sixteen honest zeros — the family books no traffic
/// per interface yet, and the node refuses to invent bytes.
unsigned long format_net_dev(char* out, unsigned long out_cap, const ProcSource& source) {
    unsigned long     used     = 0;
    const char* const kHeads[] = {
        "Inter-|   Receive                            |  Transmit\n",
        " face |bytes    packets errs drop fifo frame compressed multicast|bytes    "
        "packets errs drop fifo colls carrier compressed\n",
    };
    for (const char* const kHead : kHeads) {
        const unsigned long kLen = base::StrLength(kHead, out_cap);
        if (used + kLen >= out_cap) {
            return out_cap;
        }
        base::CopyBytes(out + used, kHead, kLen);
        used += kLen;
    }
    if (source.nth_net_iface == nullptr) {
        return used;
    }
    for (unsigned long ordinal = 0;; ++ordinal) {
        char name[kProcIfNameMax] = {};
        if (!source.nth_net_iface(source.context, ordinal, name)) {
            break;
        }
        const base::format::Arg kRow[] = {
            base::format::Str(name),
        };
        base::format::FormatToBuf(out + used, out_cap - used,
                                  "%6s: 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n",
                                  base::format::ArgsView::of(kRow));
        used += base::StrLength(out + used, out_cap - used);
        if (used >= out_cap - 1) {
            return out_cap;
        }
    }
    return used;
}

/// The name bytes plus one NUL, the whole cmdline this kernel keeps.
unsigned long format_cmdline(char* out, unsigned long out_cap, const proc::TaskSnapshot& shot) {
    const base::format::Arg kArgs[] = {
        base::format::Str(shot.name),
    };
    base::format::FormatToBuf(out, out_cap, "%s", base::format::ArgsView::of(kArgs));
    return base::StrLength(out, out_cap) + 1;
}

/// Parses a whole decimal word; false on any other shape.
bool parse_whole_decimal(std::string_view text, unsigned long& value) {
    if (text.size() > 9) {
        return false;
    }
    const base::Result<unsigned long> kParsed = base::ParseDecimal(text);
    if (!kParsed.ok()) {
        return false;
    }
    value = kParsed.value();
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
    if (ino == kMountsIno) {
        bytes = source.mounts != nullptr ? source.mounts(source.context, scratch, scratch_cap) : 0;
        return true;
    }
    if (ino == kUptimeIno) {
        const auto kNow = source.uptime_ns != nullptr ? source.uptime_ns(source.context) : 0;
        const base::format::Arg kArgs[] = {
            base::format::NumU(kNow / 1'000'000'000ULL),
            base::format::NumU((kNow / 10'000'000ULL) % 100ULL),
        };
        base::format::FormatToBuf(scratch, scratch_cap, "%llu.%02llu 0.00\n",
                                  base::format::ArgsView::of(kArgs));
        bytes = base::StrLength(scratch, scratch_cap);
        return true;
    }
    if (ino == kMeminfoIno) {
        bytes = format_meminfo(scratch, scratch_cap, source);
        return true;
    }
    if (ino == kProcDevicesIno) {
        bytes = format_devices(scratch, scratch_cap, source);
        return true;
    }
    if (ino == kProcNetDevIno) {
        bytes = format_net_dev(scratch, scratch_cap, source);
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
    const FixedName* const kFixed = base::container::FindByName(kRootNames, path);
    if (kFixed != nullptr) {
        return FsNode{
            .opaque = nullptr,
            .type   = kFixed->type,
            .ino    = kRootIno + kFixed->step,
            .size   = 0,
        };
    }
    if (path == "self/exe") {
        return FsNode{.opaque = nullptr, .type = InodeType::kFile, .ino = kProcExeIno, .size = 0};
    }
    if (path == "net/dev") {
        return FsNode{
            .opaque = nullptr,
            .type   = InodeType::kFile,
            .ino    = kProcNetDevIno,
            .size   = 0,
        };
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
    const std::string_view kLeaf        = path.substr(kSlash + 1);
    const unsigned long    kDirIno      = kProcPidBase + (tgid * kPidStride);
    const FixedName* const kPidDirEntry = base::container::FindByName(kPidDirNames, kLeaf);
    if (kPidDirEntry == nullptr) {
        return base::KernelError::kNotFound;
    }
    return FsNode{
        .opaque = nullptr,
        .type   = kPidDirEntry->type,
        .ino    = kDirIno + kPidDirEntry->step,
        .size   = 0,
    };
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
    if (node.ino == kCpuinfoIno) {
        return read_cpuinfo(source_, offset, std::span{static_cast<char*>(sink), count});
    }
    char          scratch[512] = {};
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
        if (serve_fixed(kRootNames, kRootIno, index, entry)) {
            return true;
        }
        unsigned long tgid = 0;
        if (source_.nth_tgid == nullptr ||
            !source_.nth_tgid(source_.context, index - 2 - std::size(kRootNames), tgid)) {
            return false;
        }
        const base::format::Arg kName[] = {
            base::format::NumU(tgid),
        };
        base::format::FormatToBuf(entry.name, kFsNameMax, "%lu", base::format::ArgsView::of(kName));
        entry.ino = kProcPidBase + (tgid * kPidStride);
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
    return serve_fixed(kPidDirNames, node.ino, index, entry);
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
    if (node.ino == kCpuinfoIno) {
        probed.size = cpuinfo_size(source_);
        return {};
    }
    if (node.ino == kProcExeIno) {
        probed.size = source_.exe_size != nullptr ? source_.exe_size(source_.context) : 0;
        return {};
    }
    char          scratch[512] = {};
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
    retain(node);
    return node;
}

}  // namespace cinux::fs
