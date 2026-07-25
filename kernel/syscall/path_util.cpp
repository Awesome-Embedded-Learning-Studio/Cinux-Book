/**
 * @file kernel/syscall/path_util.cpp
 * @brief Shared path resolution helper implementation
 */

#include "kernel/syscall/path_util.hpp"

#include <cinux/string_view.hpp>

#include "kernel/arch/x86_64/user_access.hpp"  // P0g (SMAP): access_ok / get_user
#include "kernel/lib/string.hpp"
#include "kernel/proc/scheduler.hpp"

namespace cinux::syscall {

bool read_user_path(uint64_t path_virt, char* out, size_t cap) {
    // access_ok rejects NULL, non-user addresses, and wraparound up front.
    if (!cinux::user::access_ok(reinterpret_cast<const void*>(path_virt), 1)) {
        return false;
    }
    // Copy the user string byte-by-byte through the SMAP accessor (get_user
    // raises AC only for the 1-byte read window). Stop at NUL or at cap-1.
    size_t len = 0;
    while (len + 1 < cap) {
        char c;
        if (!cinux::user::get_user(&c, reinterpret_cast<const char*>(path_virt + len))) {
            return false;
        }
        if (c == '\0') {
            break;
        }
        out[len++] = c;
    }
    // Verify the string actually ended within cap (no NUL => too long).
    char term = 0;
    if (!cinux::user::get_user(&term, reinterpret_cast<const char*>(path_virt + len))) {
        return false;
    }
    if (term != '\0') {
        return false;  // no NUL within cap
    }
    out[len] = '\0';
    return len > 0;  // reject empty path (matches the old resolve_user_path)
}

bool resolve_user_path(uint64_t path_virt, char* out) {
    // Stage the raw user path on the heap (PathBuf), not the kernel stack: a
    // 4 KB char[PATH_MAX] plus the canonicaliser scratch overflowed the 16 KB
    // kernel stack on the first path syscall (see kernel/fs/path.hpp PathBuf).
    cinux::fs::PathBuf raw;
    if (!read_user_path(path_virt, raw.data(), cinux::fs::PATH_MAX)) {
        return false;
    }

    cinux::proc::Task* current = cinux::proc::Scheduler::current();
    const char* cwd = (current != nullptr && current->cwd != nullptr) ? current->cwd->path : "/";

    return cinux::fs::path_resolve(cwd, raw.data(), out);
}

bool split_pathname(const char* path, char* parent_out, const char** name_out,
                    uint32_t* namelen_out) {
    using cinux::lib::StringView;

    StringView sv(path);

    // Empty path, or trailing slash — ambiguous for create/mkdir/unlink/rmdir.
    if (sv.empty() || sv.back() == '/') {
        return false;
    }

    size_t last_sep = sv.rfind('/');
    if (last_sep == StringView::npos) {
        // No separator: parent is root (empty), leaf is the whole path.
        parent_out[0] = '\0';
        *name_out     = path;
        *namelen_out  = static_cast<uint32_t>(sv.size());
    } else {
        // Parent portion [0, last_sep), NUL-terminated for VFS lookup().
        StringView parent = sv.substr(0, last_sep);
        memcpy(parent_out, parent.data(), parent.size());
        parent_out[parent.size()] = '\0';

        *name_out    = path + last_sep + 1;
        *namelen_out = static_cast<uint32_t>(sv.size() - last_sep - 1);
    }

    if (*namelen_out == 0) {
        return false;
    }
    return true;
}

}  // namespace cinux::syscall
