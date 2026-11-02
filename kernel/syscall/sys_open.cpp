#include "kernel/syscall/sys_open.hpp"

#include <string_view>

#include "cinux/assert.hpp"
#include "cinux/path.hpp"
#include "cinux/result.hpp"
#include "cinux/shared.hpp"
#include "kernel/driver/tty/tty_config.hpp"
#include "kernel/fs/file_table.hpp"
#include "kernel/fs/file_world.hpp"
#include "kernel/fs/fs_config.hpp"
#include "kernel/fs/open_file.hpp"
#include "kernel/fs/vfs.hpp"
#include "kernel/proc/task.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"

namespace cinux::syscall {


long long HandleCreat(const SyscallFrame& frame) {
    SyscallFrame flat{};
    flat.rdi = frame.rdi;
    flat.rsi = kOpenCreat | kOpenWriteOnly | kOpenTrunc;
    flat.rdx = frame.rsi;
    return HandleOpen(flat);
}

long long HandleOpen(const SyscallFrame& frame) {
    char storage[fs::kFsPathMax] = {};
    if (const long long kBad = FoldUserPath(frame.rdi, storage, fs::kFsPathMax); kBad < 0) {
        return kBad;
    }
    return OpenPath(storage, frame.rsi, frame.rdx);
}

long long OpenPath(const char* path, Word flags, Word mode) {
    const base::Result<fs::FsView> kView = fs::FileWorld::self().resolve(path);
    if (!kView.ok()) {
        return -ErrnoFrom(kView.error());
    }
    const fs::FsView& flat = kView.value();

    base::Result<fs::FsNode> node = flat.ops->lookup_held(flat.self, flat.rest);
    if (!node.ok() && node.error() == base::KernelError::kNotFound && (flags & kOpenCreat) != 0) {
        if ((flags & kOpenTrunc) != 0 && (flags & kOpenAccMode) != 0 &&
            flat.ops->truncate == nullptr) {
            return -kEopnotsupp;
        }
        node = flat.ops->make_file_held(flat.self, flat.rest);
        if (node.ok() && flat.ops->chmod != nullptr) {
            const auto kMode =
                flat.ops->chmod(flat.self, node.value(), static_cast<unsigned int>(mode) & 0777U);
            if (!kMode.ok()) {
                flat.ops->release(flat.self, node.value());
                return -ErrnoFrom(kMode.error());
            }
        }
    }
    if (!node.ok()) {
        return -ErrnoFrom(node.error());
    }
    const auto kLookedUp = node.value();
    node                 = flat.ops->adopt_open(flat.self, kLookedUp, flat.rest);
    flat.ops->release(flat.self, kLookedUp);
    if (!node.ok()) {
        return -ErrnoFrom(node.error());
    }

    const proc::Task* const           kSelf = CurrentTask();
    const base::Result<unsigned long> kFd =
        kSelf->files->alloc(flat.ops, flat.self, node.value(),
                            {
                                .status   = flags & kOpenStatusMask,
                                .cloexec  = (flags & kOpenCloexec) != 0,
                                .lifetime = flat.lifetime,
                                .limit    = FileDescriptorLimit(),
                            });
    flat.ops->release(flat.self, node.value());
    if (!kFd.ok()) {
        return -ErrnoFrom(kFd.error());
    }
    if ((flags & kOpenTrunc) != 0 && (flags & kOpenAccMode) != 0 &&
        node.value().type == fs::InodeType::kFile) {
        const base::Result<void> kCut = flat.ops->truncate != nullptr
                                            ? flat.ops->truncate(flat.self, node.value())
                                            : base::KernelError::kNotSupported;
        if (!kCut.ok()) {
            cinux::base::safety::Check(kSelf->files->free(kFd.value()).ok(),
                                       "open lost its descriptor");
            return -ErrnoFrom(kCut.error());
        }
    }

    if (node.value().type == fs::InodeType::kCharDevice && (flags & kOpenNoctty) == 0 &&
        kSelf->controlling_tty == nullptr && kSelf->sid == kSelf->tgid &&
        flat.ops->ioctl != nullptr) {
        flat.ops->ioctl(flat.self, node.value(), driver::kTtyTiocsctty, 0);
    }
    return static_cast<long long>(kFd.value());
}

namespace {

/// Creates the leaf of one relative path under the held directory,
/// walking the anchor's own nodes for the parent stretch.
base::Result<fs::FsNode> create_under_anchor(const fs::OpenFile& anchor, std::string_view relative,
                                             Word mode) {
    std::string_view parent{};
    std::string_view leaf{};
    static_cast<void>(base::SplitLeaf(relative, parent, leaf));
    base::Result<fs::FsNode> base_node = anchor.node;
    if (!parent.empty()) {
        base_node = anchor.ops->lookup_at(anchor.backend, anchor.node, parent);
    }
    if (!base_node.ok()) {
        return base_node;
    }
    if (anchor.ops->make_file_at == nullptr) {
        if (!parent.empty()) {
            anchor.ops->release(anchor.backend, base_node.value());
        }
        return base::KernelError::kNotSupported;
    }
    base::Result<fs::FsNode> made =
        anchor.ops->make_file_at(anchor.backend, base_node.value(), leaf);
    if (!parent.empty()) {
        anchor.ops->release(anchor.backend, base_node.value());
    }
    if (made.ok() && anchor.ops->chmod != nullptr) {
        const auto kMode = anchor.ops->chmod(anchor.backend, made.value(),
                                             static_cast<unsigned int>(mode) & 0777U);
        if (!kMode.ok()) {
            anchor.ops->release(anchor.backend, made.value());
            return kMode.error();
        }
    }
    return made;
}

}  // namespace

long long OpenPathAt(Word descriptor, const char* relative, Word flags, Word mode) {
    const base::Result<base::Shared<fs::OpenFile>> kDir = LiveSlot(descriptor);
    if (!kDir.ok()) {
        return -kEbadf;
    }
    const base::Shared<fs::OpenFile>& anchor = kDir.value();
    if (anchor->node.type != fs::InodeType::kDirectory) {
        return -kEnotdir;
    }
    if ((flags & kOpenTrunc) != 0 && (flags & kOpenAccMode) != 0 &&
        anchor->ops->truncate == nullptr) {
        return -kEopnotsupp;
    }
    base::Result<fs::FsNode> node = anchor->ops->lookup_at(anchor->backend, anchor->node, relative);
    if (!node.ok() && node.error() == base::KernelError::kNotFound && (flags & kOpenCreat) != 0) {
        node = create_under_anchor(*anchor, relative, mode);
    }
    if (!node.ok()) {
        return node.error() == base::KernelError::kNotSupported ? -kEopnotsupp
                                                                : -ErrnoFrom(node.error());
    }
    const auto kLookedUp = node.value();
    node                 = anchor->ops->adopt_open(anchor->backend, kLookedUp, relative);
    anchor->ops->release(anchor->backend, kLookedUp);
    if (!node.ok()) {
        return -ErrnoFrom(node.error());
    }

    const proc::Task* const           kSelf = CurrentTask();
    const base::Result<unsigned long> kFd =
        kSelf->files->alloc(anchor->ops, anchor->backend, node.value(),
                            {
                                .status   = flags & kOpenStatusMask,
                                .cloexec  = (flags & kOpenCloexec) != 0,
                                .lifetime = anchor->lifetime,
                                .limit    = FileDescriptorLimit(),
                            });
    anchor->ops->release(anchor->backend, node.value());
    if (!kFd.ok()) {
        return -ErrnoFrom(kFd.error());
    }
    if ((flags & kOpenTrunc) != 0 && (flags & kOpenAccMode) != 0 &&
        node.value().type == fs::InodeType::kFile) {
        const base::Result<void> kCut = anchor->ops->truncate(anchor->backend, node.value());
        if (!kCut.ok()) {
            cinux::base::safety::Check(kSelf->files->free(kFd.value()).ok(),
                                       "open lost its descriptor");
            return -ErrnoFrom(kCut.error());
        }
    }
    return static_cast<long long>(kFd.value());
}

}  // namespace cinux::syscall
