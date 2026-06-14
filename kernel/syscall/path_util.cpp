/**
 * @file kernel/syscall/path_util.cpp
 * @brief Shared path resolution helper implementation
 */

#include "kernel/syscall/path_util.hpp"

#include <cinux/string_view.hpp>

#include "kernel/lib/string.hpp"
#include "kernel/proc/scheduler.hpp"

namespace cinux::syscall {

bool resolve_user_path(uint64_t path_virt, char* out) {
    if (!validate_user_ptr(path_virt)) {
        return false;
    }

    auto* path = reinterpret_cast<const char*>(path_virt);

    if (path[0] == '\0') {
        return false;
    }

    cinux::proc::Task* current = cinux::proc::Scheduler::current();
    const char*        cwd     = (current != nullptr) ? current->cwd : "/";

    return cinux::fs::path_resolve(cwd, path, out);
}

bool split_pathname(const char* path, char* parent_out,
                    const char** name_out, uint32_t* namelen_out) {
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
