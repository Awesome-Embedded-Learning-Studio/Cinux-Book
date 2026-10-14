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

#include "cinux/own.hpp"
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


bool ReadWholeFile(const FsView& view, base::Own<unsigned char[]>& out, unsigned long& out_bytes) {
    const base::Result<FsNode> kNode = view.ops->lookup(view.self, view.rest);
    if (!kNode.ok()) {
        return false;
    }
    FsStat                   probe{};
    const base::Result<void> kProbed = view.ops->stat(view.self, kNode.value(), probe);
    if (!kProbed.ok()) {
        return false;
    }
    base::Own<unsigned char[]> buffer(new unsigned char[probe.size]);
    if (!buffer.holds()) {
        return false;
    }
    const base::Result<unsigned long> kGot =
        view.ops->read(view.self, kNode.value(), 0, buffer.get(), probe.size);
    if (!kGot.ok() || kGot.value() != probe.size) {
        return false;
    }
    out       = static_cast<base::Own<unsigned char[]>&&>(buffer);
    out_bytes = probe.size;
    return true;
}

}  // namespace cinux::fs
