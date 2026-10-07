/**
 * @file    vfs.cpp
 * @brief   The mount table walk: longest covering prefix wins.
 *
 * resolve() scans every live slot, keeps the deepest prefix that really
 * ends on a component boundary, and strips exactly one slash after it —
 * the empty rest means the mount point itself.
 *
 * @author  Charliechen114514
 * @date    2026-10-07
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_fs
 * @copyright Copyright (c) 2026
 */

#include "kernel/fs/vfs.hpp"

#include <string_view>

#include "cinux/result.hpp"

namespace cinux::fs {

base::Result<FsView> Vfs::resolve(std::string_view path) const {
    if (path.empty() || path.front() != '/') {
        return base::KernelError::kInvalidArgument;
    }

    const Slot* best = nullptr;
    slots_.for_each([&best, path]([[maybe_unused]] unsigned long index, const Slot& slot) {
        const std::string_view kPrefix(slot.prefix, slot.prefix_len);
        if (!path.starts_with(kPrefix)) {
            return;
        }
        if (kPrefix.size() > 1 && path.size() > kPrefix.size() && path[kPrefix.size()] != '/') {
            return;
        }
        if (best == nullptr || slot.prefix_len > best->prefix_len) {
            best = &slot;
        }
    });
    if (best == nullptr) {
        return base::KernelError::kNotFound;
    }

    unsigned long rest_start = best->prefix_len;
    if (rest_start < path.size() && path[rest_start] == '/') {
        ++rest_start;
    }
    return FsView{.ops = best->ops, .self = best->self, .rest = path.substr(rest_start)};
}

}  // namespace cinux::fs
