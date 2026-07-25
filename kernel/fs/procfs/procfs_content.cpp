/**
 * @file kernel/fs/procfs/procfs_content.cpp
 * @brief ProcFS pseudo-file content generation: snapshot -> text (F6-M2)
 *
 * Implements the stat/cmdline/meminfo/cpuinfo formatters declared in
 * procfs_content.hpp.  All rendering goes through cinux::fmt::format ({fmt}-
 * style {} placeholders, type-safe variadic) -- the old hand-rolled
 * LineBuilder `b.put_s / b.put_u / b.put` eyesore is gone.
 */

#include "procfs_content.hpp"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "kernel/lib/format.hpp"           // cinux::fmt::format ({} placeholders)
#include "kernel/proc/process.hpp"         // TaskState (task_state_char)
#include "kernel/proc/task_snapshot.hpp"   // TaskSnapshot
#include "kernel/version.hpp"              // kCpuModel / kOsVersion

namespace cinux::fs {
namespace {

/// Map a TaskState to the single Linux /proc/<pid>/stat state character.
char task_state_char(cinux::proc::TaskState s) {
    switch (s) {
    case cinux::proc::TaskState::Running:
    case cinux::proc::TaskState::Ready:
        return 'R';  // runnable
    case cinux::proc::TaskState::Blocked:
        return 'S';  // sleeping
    case cinux::proc::TaskState::Stopped:
        return 'T';
    case cinux::proc::TaskState::Zombie:
        return 'Z';
    case cinux::proc::TaskState::Dead:
        return 'X';
    }
    return '?';
}

}  // namespace

uint32_t format_proc_stat(const cinux::proc::TaskSnapshot& s, char* buf, uint32_t cap) {
    // "pid (name) state ppid tgid uid gid\n" -- a documented subset of Linux's
    // /proc/<pid>/stat (Cinux keeps no per-task accounting fields).
    return cinux::fmt::format(buf, cap, "{} ({}) {} {} {} {} {}\n",
                              static_cast<uint32_t>(s.pid), s.name,
                              task_state_char(s.state),
                              static_cast<uint32_t>(s.ppid),
                              static_cast<uint32_t>(s.tgid), s.uid, s.gid);
}

uint32_t format_proc_cmdline(const cinux::proc::TaskSnapshot& s, char* dst, uint32_t cap) {
    // /proc/<pid>/cmdline is NUL-separated argv; Cinux keeps no argv, so the
    // task name (comm) is exposed as a best-effort single field.  Length
    // includes the terminating NUL (the cmdline contract).
    uint32_t len = cinux::fmt::format(dst, cap, "{}", s.name);
    return len < cap ? len + 1 : cap;
}

uint32_t format_proc_meminfo(uint32_t total_kb, uint32_t free_kb, char* buf, uint32_t cap) {
    // busybox `free` greps MemTotal/MemFree/MemAvailable/Buffers/Cached; Cinux
    // tracks no buffer/cache/swap, so those read 0 (honest, not fabricated).
    return cinux::fmt::format(buf, cap,
                              "MemTotal: {} kB\n"
                              "MemFree: {} kB\n"
                              "MemAvailable: {} kB\n"
                              "Buffers: {} kB\n"
                              "Cached: {} kB\n"
                              "SwapTotal: {} kB\n"
                              "SwapFree: {} kB\n",
                              total_kb, free_kb, free_kb, 0u, 0u, 0u, 0u);
}

uint32_t format_proc_cpuinfo(uint32_t cpu_count, const uint8_t* apic_ids, char* buf,
                             uint32_t cap) {
    // One "processor / apicid / model name" block per CPU, blank-line
    // separated.  busybox `nproc` counts "processor :" lines (case-insensitive);
    // `cat /proc/cpuinfo` shows the per-CPU view.  @p apic_ids is the MADT list
    // (null in synthetic contexts -> sequential ids fallback).
    uint32_t len = 0;
    for (uint32_t i = 0; i < cpu_count; ++i) {
        uint32_t apic = apic_ids != nullptr ? apic_ids[i] : i;
        bool     last = (i + 1 == cpu_count);
        len += cinux::fmt::format(buf + len, cap - len,
                                  "processor\t: {}\n"
                                  "apicid\t\t: {}\n"
                                  "model name\t: {} {}{}",
                                  i, apic, cinux::kCpuModel, cinux::kOsVersion,
                                  last ? "\n" : "\n\n");
    }
    return len;
}

}  // namespace cinux::fs
