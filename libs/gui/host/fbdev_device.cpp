/**
 * @file host/fbdev_device.cpp
 * @brief Linux fbdev backend implementation (open/ioctl/mmap + blit)
 *
 * Host-side ONLY. Uses <linux/fb.h> ioctl(FBIOGET_VSCREENINFO/FIXEDSCREENINFO)
 * + mmap. The core never includes this.
 */

#include "fbdev_device.hpp"

#include <cstdint>
#include <cstring>

#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace cinux::gui {

FbdevDevice::~FbdevDevice() {
    close();
}

bool FbdevDevice::open(const char* path) {
    fd_ = ::open(path, O_RDWR);
    if (fd_ < 0) {
        return false;
    }

    fb_var_screeninfo vinfo{};
    fb_fix_screeninfo finfo{};
    if (::ioctl(fd_, FBIOGET_VSCREENINFO, &vinfo) < 0 ||
        ::ioctl(fd_, FBIOGET_FSCREENINFO, &finfo) < 0) {
        close();
        return false;
    }

    if (vinfo.bits_per_pixel != 32u) {
        // P1 supports the XRGB8888 path only; other layouts stay !ready.
        close();
        return false;
    }

    width_   = vinfo.xres;
    height_  = vinfo.yres;
    stride_  = finfo.line_length;
    map_len_ = finfo.smem_len;
    format_  = PixelFormat::kXrgb8888;

    void* m = ::mmap(nullptr, map_len_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (m == MAP_FAILED) {
        map_ = nullptr;
        close();
        return false;
    }
    map_ = m;
    return true;
}

void FbdevDevice::close() {
    if (map_ != nullptr) {
        ::munmap(map_, map_len_);
        map_ = nullptr;
    }
    map_len_ = 0;
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

void FbdevDevice::blit_rect(int x, int y, int w, int h, const void* src, uint32_t src_stride) {
    if (map_ == nullptr || src == nullptr) {
        return;
    }
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (w <= 0 || h <= 0) {
        return;
    }
    if (static_cast<uint32_t>(x) >= width_ || static_cast<uint32_t>(y) >= height_) {
        return;
    }
    if (static_cast<uint32_t>(x + w) > width_) {
        w = static_cast<int>(width_) - x;
    }
    if (static_cast<uint32_t>(y + h) > height_) {
        h = static_cast<int>(height_) - y;
    }
    if (w <= 0 || h <= 0) {
        return;
    }

    const uint8_t* src_bytes   = static_cast<const uint8_t*>(src);
    uint8_t*       dst_bytes   = static_cast<uint8_t*>(map_);
    const size_t   row_bytes   = static_cast<size_t>(w) * 4u;
    for (int row = 0; row < h; row++) {
        const size_t src_off = static_cast<size_t>(y + row) * src_stride +
                               static_cast<size_t>(x) * 4u;
        const size_t dst_off = static_cast<size_t>(y + row) * stride_ +
                               static_cast<size_t>(x) * 4u;
        std::memcpy(dst_bytes + dst_off, src_bytes + src_off, row_bytes);
    }
}

}  // namespace cinux::gui
