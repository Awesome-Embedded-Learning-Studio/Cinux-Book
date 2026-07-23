/**
 * @file kernel/fs/procfs_content.hpp
 * @brief ProcFS pseudo-file content generation (F6-M2)
 *
 * Generates the text bodies of the /proc/<pid>/{stat,cmdline} pseudo-files from
 * the live Task fields.  Split out of procfs.cpp so procfs.cpp (the FS plumbing
 * -- inode pools, ops, lookup) stays under the 500-line file cap; this module is
 * the "render a Task to text" concern alone.
 *
 * The formatters are kernel-linked (they read Task) and therefore not part of
 * the host unit tests.
 *
 * Namespace: cinux::fs
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace cinux::proc {
struct Task;  // full definition in kernel/proc/process.hpp
}

namespace cinux::fs {

/// Buffer cap for a generated /proc/<pid>/stat line.  The layout is
/// "pid (name) state ppid tgid uid gid\n"; name is bounded, so the whole line
/// fits well under 96 bytes.
static constexpr uint32_t kProcStatLineMax = 96;

/// Buffer cap for /proc/<pid>/cmdline content (task name + NUL).
static constexpr uint32_t kProcCmdlineMax = 64;

/// Format a simplified /proc/<pid>/stat line into @p buf (NUL-terminated).
/// Layout: "pid (name) state ppid tgid uid gid\n".  This is a documented subset
/// of Linux's /proc/<pid>/stat (CinuxOS keeps no per-task accounting fields).
/// @return content length in bytes (excluding the NUL).
uint32_t format_proc_stat(const cinux::proc::Task* t, char* buf, uint32_t cap);

/// Copy the task name (bounded) into @p dst and append a NUL; return the length
/// including the NUL.  /proc/<pid>/cmdline is NUL-separated argv on Linux;
/// CinuxOS keeps no argv, so the task name (comm) is exposed as a best-effort
/// single field.
uint32_t format_proc_cmdline(const cinux::proc::Task* t, char* dst, uint32_t cap);

}  // namespace cinux::fs
