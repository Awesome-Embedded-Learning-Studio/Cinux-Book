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
#include <utility>

#include "cinux/format.hpp"
#include "cinux/math.hpp"
#include "cinux/memory.hpp"
#include "cinux/own.hpp"
#include "cinux/result.hpp"
#include "cinux/shared.hpp"
#include "kernel/arch/x86_64/irq_guard.hpp"
#include "kernel/fs/mount_lease.hpp"

namespace cinux::fs {

base::Result<FsView> Vfs::resolve(std::string_view path) const {
    const arch::IrqGuard kIrq;
    const auto           kGuard = gate_.guard();
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
    base::Shared<MountLease> lifetime;
    if (best->lifetime != nullptr) {
        best->lifetime->retain();
        lifetime = base::Shared<MountLease>(best->lifetime);
    }
    return FsView{
        .ops      = best->ops,
        .self     = best->self,
        .rest     = path.substr(rest_start),
        .lifetime = std::move(lifetime),
    };
}


unsigned long Vfs::mounts_text(char* out, unsigned long capacity) const {
    const arch::IrqGuard kIrq;
    const auto           kGuard = gate_.guard();
    unsigned long        used   = 0;
    slots_.for_each([&]([[maybe_unused]] unsigned long index, const Slot& slot) {
        if (capacity <= used + 1) {
            return;
        }
        const base::format::Arg kArgs[] = {
            base::format::Str("none"),
            base::format::Str(slot.prefix),
            base::format::Str(slot.type_name),
        };
        base::format::FormatToBuf(out + used, capacity - used, "%s %s %s rw 0 0\n",
                                  base::format::ArgsView::of(kArgs));
        used += base::StrLength(out + used, capacity - used);
    });
    return used;
}

base::Result<void> ReadWholeFile(const FsView& view, base::Own<unsigned char[]>& out,
                                 unsigned long& out_bytes) {
    return ReadFilePrefix(view, ~0UL, out, out_bytes);
}

base::Result<void> ReadFilePrefix(const FsView& view, unsigned long limit,
                                  base::Own<unsigned char[]>& out, unsigned long& out_bytes) {
    const base::Result<FsNode> kNode = view.ops->lookup(view.self, view.rest);
    if (!kNode.ok()) {
        return kNode.error();
    }
    FsStat                   probe{};
    const base::Result<void> kProbed = view.ops->stat(view.self, kNode.value(), probe);
    if (!kProbed.ok()) {
        return kProbed.error();
    }
    const auto                 kBytes = base::math::Min(probe.size, limit);
    base::Own<unsigned char[]> buffer(new unsigned char[kBytes]);
    if (!buffer.holds()) {
        return base::KernelError::kOutOfMemory;
    }
    const base::Result<unsigned long> kGot =
        view.ops->read(view.self, kNode.value(), 0, buffer.get(), kBytes);
    if (!kGot.ok()) {
        return kGot.error();
    }
    if (kGot.value() != kBytes) {
        return base::KernelError::kIOError;
    }
    out       = static_cast<base::Own<unsigned char[]>&&>(buffer);
    out_bytes = kBytes;
    return {};
}

}  // namespace cinux::fs
