/** @file kernel/fs/procfs/procfs_pseudo.cpp
 * @brief ProcFS system pseudo-file InodeOps (/proc/meminfo, /proc/cpuinfo)
 *
 * Split out of procfs.cpp (F4 SMP) once /proc/cpuinfo joined /proc/meminfo --
 * keeping both there pushed procfs.cpp past the 500-line cap.  Each op renders
 * via a generator in procfs_content.cpp and serves it read-once through
 * copy_pseudo.  procfs.cpp wires them up through the procfs_new_*_ops()
 * factories below so the InodeOps subclass definitions don't leak across TUs.
 */

#include "procfs.hpp"

#include <stddef.h>
#include <stdint.h>

#include "kernel/drivers/acpi/acpi.hpp"  // g_acpi_info (/proc/cpuinfo)
#include "kernel/lib/string.hpp"
#include "kernel/mm/pmm.hpp"             // g_pmm (/proc/meminfo)
#include "procfs_content.hpp"

namespace cinux::fs {

using cinux::lib::Error;
using cinux::lib::ErrorOr;

namespace {

/// Stat for a read-only (0444) ProcFS regular pseudo-file.
void fill_file_stat(const Inode* inode, struct stat* st) {
    memset(st, 0, sizeof(*st));
    st->st_ino     = inode->ino;
    st->st_nlink   = 1;
    st->st_mode    = kProcSIfReg | 0444;
    st->st_blksize = 4096;
}

/// Copy min(count, len-offset) bytes of generated content (0 past end = EOF).
ErrorOr<int64_t> copy_pseudo(const char* content, uint32_t len, uint64_t offset, void* buf,
                             uint64_t count) {
    if (offset >= len) {
        return 0;
    }
    uint64_t avail = static_cast<uint64_t>(len) - offset;
    uint64_t n     = count < avail ? count : avail;
    memcpy(buf, content + offset, n);
    return static_cast<int64_t>(n);
}

// F-ECO busybox: /proc/meminfo -- read regenerates the content from g_pmm so
// `free` (which greps MemTotal/MemFree/MemAvailable/Buffers/Cached) works.
class ProcMeminfoFileOps : public InodeOps {
public:
    ErrorOr<int64_t> read(const Inode* inode, uint64_t offset, void* buf, uint64_t count) override {
        if (inode == nullptr || buf == nullptr) {
            return Error::InvalidArgument;
        }
        constexpr uint32_t kBytesPerKb = 1024;
        constexpr uint32_t kPageBytes  = 4096;
        uint32_t           total_kb =
            static_cast<uint32_t>(cinux::mm::g_pmm.total_page_count() * kPageBytes / kBytesPerKb);
        uint32_t free_kb =
            static_cast<uint32_t>(cinux::mm::g_pmm.free_page_count() * kPageBytes / kBytesPerKb);
        char     line[kProcMeminfoMax];
        uint32_t len = format_proc_meminfo(total_kb, free_kb, line, sizeof(line));
        return copy_pseudo(line, len, offset, buf, count);
    }
    ErrorOr<void> stat(const Inode* inode, struct stat* st) override {
        if (inode == nullptr || st == nullptr) {
            return Error::InvalidArgument;
        }
        fill_file_stat(inode, st);
        return {};
    }
};

// F4 SMP: /proc/cpuinfo -- read regenerates content from g_acpi_info so
// `cat /proc/cpuinfo` and busybox `nproc` report the real CPU topology.
class ProcCpuinfoFileOps : public InodeOps {
public:
    ErrorOr<int64_t> read(const Inode* inode, uint64_t offset, void* buf, uint64_t count) override {
        if (inode == nullptr || buf == nullptr) {
            return Error::InvalidArgument;
        }
        const auto& info   = cinux::drivers::acpi::g_acpi_info;
        char        line[kProcCpuinfoMax];
        uint32_t    len = format_proc_cpuinfo(info.cpu_count, info.cpu_apic_ids, line, sizeof(line));
        return copy_pseudo(line, len, offset, buf, count);
    }
    ErrorOr<void> stat(const Inode* inode, struct stat* st) override {
        if (inode == nullptr || st == nullptr) {
            return Error::InvalidArgument;
        }
        fill_file_stat(inode, st);
        return {};
    }
};

}  // namespace

/// @name Pseudo-file ops factories (called by procfs.cpp mount()).
///@{
InodeOps* procfs_new_meminfo_ops() {
    return new ProcMeminfoFileOps();
}
InodeOps* procfs_new_cpuinfo_ops() {
    return new ProcCpuinfoFileOps();
}
///@}

}  // namespace cinux::fs
