/**
 * @file kernel/drivers/video/fb_dev.hpp
 * @brief /dev/fb0 InodeOps: device mmap + ioctl (F-GUI-USERSPACE batch 1)
 *
 * Kernel-only adapter over the system Framebuffer singleton.  mmap binds a
 * user VMA to the VBE framebuffer physical memory (the page-fault handler
 * maps it uncached via the IoPhys VMA kind); ioctl(FBIOGET_SCREENINFO)
 * reports geometry so a userspace GUI host can size its surface.
 *
 * Namespace: cinux::drivers
 */

#pragma once

#include <cinux/expected.hpp>
#include <cstdint>

#include "kernel/fs/inode.hpp"

namespace cinux::drivers {

/// /dev/fb0 ops.  See fb_dev.cpp for the mmap/ioctl semantics.
class FramebufferDevOps : public cinux::fs::InodeOps {
public:
    cinux::lib::ErrorOr<uint64_t> mmap(const cinux::fs::Inode* inode, uint64_t offset,
                                       uint64_t length) override;
    cinux::lib::ErrorOr<int64_t>  ioctl(const cinux::fs::Inode* inode, uint32_t request,
                                        uint64_t arg) override;
};

/// Global /dev/fb0 ops instance, registered by devfs::init().
cinux::fs::InodeOps& framebuffer_dev_ops();

/// ioctl request: read screen geometry (width/height/pitch/bpp).  Mirrors
/// Linux FBIOGET_VSCREENINFO in role, not in struct layout -- userspace
/// mirrors this constant.
constexpr uint32_t kFbioGetScreenInfo = 0x4600;

}  // namespace cinux::drivers
