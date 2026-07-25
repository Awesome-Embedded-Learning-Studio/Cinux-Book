/**
 * @file kernel/drivers/video/fb_dev.cpp
 * @brief /dev/fb0 character device: mmap + ioctl (F-GUI-USERSPACE batch 1)
 *
 * Thin InodeOps adapter over the system Framebuffer singleton.  Kernel-only
 * (linked into big_kernel_common, NOT host unit tests) -- it pulls
 * copy_to_user + InodeOps, so it is kept out of framebuffer.cpp which stays
 * host-linkable (mirrors the devfs.cpp / devfs_init.cpp split).
 *
 * Namespace: cinux::drivers
 */

#include "kernel/drivers/video/fb_dev.hpp"

#include <stdint.h>

#include "kernel/arch/x86_64/user_access.hpp"  // copy_to_user
#include "kernel/drivers/video/framebuffer.hpp"
#include "kernel/fs/inode.hpp"

namespace cinux::drivers {

namespace {
// Simplified screen-info reply: geometry + format.  Deliberately NOT Linux
// fb_var_screeninfo (a ~160-byte struct of fields we do not model) -- we
// expose only what a Cinux GUI host needs.
struct FbScreenInfo {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;  // bytes per scan line
    uint32_t bpp;    // bits per pixel
};
}  // namespace

cinux::lib::ErrorOr<uint64_t> FramebufferDevOps::mmap(const cinux::fs::Inode*, uint64_t offset,
                                                      uint64_t length) {
    Framebuffer* fb = system_framebuffer();
    if (fb == nullptr) {
        return cinux::lib::Error::NotImplemented;  // no framebuffer initialised
    }
    // Bound the mapping to the actual screen memory.  Guard against overflow
    // before the subtraction.
    if (offset > fb->size() || length > fb->size() - offset) {
        return cinux::lib::Error::InvalidArgument;
    }
    return fb->phys_base() + offset;
}

cinux::lib::ErrorOr<int64_t> FramebufferDevOps::ioctl(const cinux::fs::Inode*, uint32_t request,
                                                      uint64_t arg) {
    if (request != kFbioGetScreenInfo) {
        return cinux::lib::Error::NotImplemented;  // sys_ioctl maps this to -ENOTTY
    }
    Framebuffer* fb = system_framebuffer();
    if (fb == nullptr) {
        return cinux::lib::Error::NotImplemented;
    }
    FbScreenInfo info{};
    info.width  = fb->width();
    info.height = fb->height();
    info.pitch  = fb->pitch();
    info.bpp    = 32;  // VBE mode 0x144 is 32-bpp XRGB; Framebuffer assumes it
    if (!cinux::user::copy_to_user(reinterpret_cast<void*>(arg), &info, sizeof(info))) {
        return cinux::lib::Error::Fault;
    }
    return 0;
}

namespace {
FramebufferDevOps g_fb_dev_ops;
}

cinux::fs::InodeOps& framebuffer_dev_ops() {
    return g_fb_dev_ops;
}

}  // namespace cinux::drivers
