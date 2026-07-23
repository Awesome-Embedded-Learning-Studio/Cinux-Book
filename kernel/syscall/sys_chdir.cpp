/**
 * @file kernel/syscall/sys_chdir.cpp
 * @brief sys_chdir handler implementation
 *
 * Resolves the given path (relative or absolute), verifies it is a
 * directory, and updates the current task's cwd field.
 */

#include "kernel/syscall/sys_chdir.hpp"

#include <stdint.h>

#include "kernel/errno.hpp"
#include "kernel/fs/path.hpp"
#include "kernel/fs/vfs_mount.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/lib/string.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/syscall/path_util.hpp"

namespace cinux::syscall {

namespace {

using cinux::lib::kprintf;

}  // anonymous namespace

int64_t do_chdir_kernel(const char* resolved_path) {
    // Step 1: Resolve through the VFS mount table
    const char*            rel_path = nullptr;
    cinux::fs::FileSystem* fs       = cinux::fs::vfs_resolve(resolved_path, &rel_path);

    if (fs == nullptr) {
        kprintf("[SYS_CHDIR] No filesystem mounted for '%s'\n", resolved_path);
        return -kEnoent;
    }

    // Step 2: Look up the inode
    auto inode_result = fs->lookup(rel_path);
    if (!inode_result.ok()) {
        kprintf("[SYS_CHDIR] Path not found: '%s'\n", resolved_path);
        return -to_errno(inode_result.error());
    }
    cinux::fs::Inode* inode = inode_result.value();

    // Step 3: Verify it is a directory
    if (inode->type != cinux::fs::InodeType::Directory) {
        kprintf("[SYS_CHDIR] Not a directory: '%s'\n", resolved_path);
        return -kEnotdir;
    }

    // Step 4: Update cwd (in place on the refcounted SharedCwd; CLONE_FS
    // sharers see the change, a forked private copy is mutated alone).
    cinux::proc::Task* current = cinux::proc::Scheduler::current();
    if (current == nullptr) {
        return -kEio;
    }
    if (current->cwd == nullptr) {
        current->cwd = cinux::proc::SharedCwd::create(resolved_path);
        return (current->cwd != nullptr) ? 0 : -12;  // ENOMEM
    }

    uint32_t i = 0;
    while (resolved_path[i] != '\0' && i + 1 < cinux::proc::SharedCwd::kPathMax) {
        current->cwd->path[i] = resolved_path[i];
        ++i;
    }
    current->cwd->path[i] = '\0';

    return 0;
}

int64_t sys_chdir(uint64_t path_virt, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
    // Boundary: resolve the user path (cwd-aware), then run kernel logic.
    cinux::fs::PathBuf resolved;
    if (!resolve_user_path(path_virt, resolved.data())) {
        return -kEfault;
    }
    return do_chdir_kernel(resolved.data());
}

}  // namespace cinux::syscall
