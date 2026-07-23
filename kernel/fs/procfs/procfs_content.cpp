/**
 * @file kernel/fs/procfs_content.cpp
 * @brief ProcFS pseudo-file content generation: Task -> text (F6-M2)
 *
 * Implements the stat/cmdline formatters declared in procfs_content.hpp.  A
 * bounded LineBuilder replaces <string>; every append stops at the cap, so the
 * output is always NUL-terminable.
 */

#include "procfs_content.hpp"

#include <stddef.h>
#include <stdint.h>

#include "kernel/lib/string.hpp"          // utoa
#include "kernel/proc/process.hpp"        // TaskState (task_state_char)
#include "kernel/proc/task_snapshot.hpp"  // TaskSnapshot

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

/// A bounded string builder for generating pseudo-file text without <string>.
/// Every append silently stops at the cap, so output is always NUL-terminable.
struct LineBuilder {
    char*    p;
    uint32_t cap;
    uint32_t len{0};

    void put(char c) {
        if (len + 1 < cap) {
            p[len++] = c;
        }
    }
    void put_s(const char* s) {
        if (s == nullptr) {
            return;
        }
        while (*s != '\0' && len + 1 < cap) {
            p[len++] = *s++;
        }
    }
    void put_u(uint32_t v) {
        char tmp[11];
        utoa(tmp, v);
        put_s(tmp);
    }
    /// Ensure a terminating NUL; return content length (excluding it).
    uint32_t finish() {
        if (len < cap) {
            p[len] = '\0';
        } else {
            p[cap - 1] = '\0';
        }
        return len;
    }
};

}  // anonymous namespace

uint32_t format_proc_stat(const cinux::proc::TaskSnapshot& s, char* buf, uint32_t cap) {
    LineBuilder b{buf, cap};
    b.put_u(static_cast<uint32_t>(s.pid));
    b.put(' ');
    b.put('(');
    b.put_s(s.name);  // snapshot name is always NUL-terminated and bounded
    b.put(')');
    b.put(' ');
    b.put(task_state_char(s.state));
    b.put(' ');
    b.put_u(static_cast<uint32_t>(s.ppid));
    b.put(' ');
    b.put_u(static_cast<uint32_t>(s.tgid));
    b.put(' ');
    b.put_u(s.uid);
    b.put(' ');
    b.put_u(s.gid);
    b.put('\n');
    return b.finish();
}

uint32_t format_proc_cmdline(const cinux::proc::TaskSnapshot& s, char* dst, uint32_t cap) {
    LineBuilder b{dst, cap};
    b.put_s(s.name);  // snapshot name is always NUL-terminated and bounded
    b.put('\0');      // cmdline always ends with a NUL
    // Length includes the terminating NUL (count the bytes actually written).
    return b.len < cap ? b.len + 1 : cap;
}

uint32_t format_proc_meminfo(uint32_t total_kb, uint32_t free_kb, char* buf, uint32_t cap) {
    // busybox `free` greps these keys (alignment is irrelevant to the parser).
    // CinuxOS tracks no buffers/cached/swap, so those read 0 -- honest, not
    // fabricated; the "used" column free prints is total-free, "buff/cache" 0.
    LineBuilder b{buf, cap};
    b.put_s("MemTotal:");
    b.put(' ');
    b.put_u(total_kb);
    b.put_s(" kB\n");
    b.put_s("MemFree:");
    b.put(' ');
    b.put_u(free_kb);
    b.put_s(" kB\n");
    b.put_s("MemAvailable:");
    b.put(' ');
    b.put_u(free_kb);
    b.put_s(" kB\n");
    b.put_s("Buffers:");
    b.put(' ');
    b.put_u(0);
    b.put_s(" kB\n");
    b.put_s("Cached:");
    b.put(' ');
    b.put_u(0);
    b.put_s(" kB\n");
    b.put_s("SwapTotal:");
    b.put(' ');
    b.put_u(0);
    b.put_s(" kB\n");
    b.put_s("SwapFree:");
    b.put(' ');
    b.put_u(0);
    b.put_s(" kB\n");
    return b.finish();
}

}  // namespace cinux::fs
