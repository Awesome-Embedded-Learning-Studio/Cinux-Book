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

#include "kernel/lib/string.hpp"    // utoa
#include "kernel/proc/process.hpp"  // Task / TaskState

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

uint32_t format_proc_stat(const cinux::proc::Task* t, char* buf, uint32_t cap) {
    LineBuilder b{buf, cap};
    b.put_u(static_cast<uint32_t>(t->pid));
    b.put(' ');
    b.put('(');
    b.put_s(t->name != nullptr ? t->name : "");
    b.put(')');
    b.put(' ');
    b.put(task_state_char(t->state));
    b.put(' ');
    b.put_u(static_cast<uint32_t>(t->ppid));
    b.put(' ');
    b.put_u(static_cast<uint32_t>(t->tgid));
    b.put(' ');
    b.put_u(t->uid);
    b.put(' ');
    b.put_u(t->gid);
    b.put('\n');
    return b.finish();
}

uint32_t format_proc_cmdline(const cinux::proc::Task* t, char* dst, uint32_t cap) {
    LineBuilder b{dst, cap};
    const char* name = (t != nullptr && t->name != nullptr) ? t->name : "";
    b.put_s(name);
    b.put('\0');  // cmdline always ends with a NUL
    // Length includes the terminating NUL (count the bytes actually written).
    return b.len < cap ? b.len + 1 : cap;
}

}  // namespace cinux::fs
