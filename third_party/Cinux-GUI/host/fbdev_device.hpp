/**
 * @file host/fbdev_device.hpp
 * @brief Linux fbdev backend -- mmap /dev/fb0 as the display surface
 *
 * Host-side ONLY (Linux fbdev ioctl/mmap). The core never sees this -- it just
 * calls host->core.flush(area, pixels, stride, fmt), and this device's flush
 * callback memcpy's the dirty rect into the mmap'd framebuffer.
 *
 * P1 Probe-1: the second real host (after the Cinux kernel), proving the Host
 * ABI seam is clean -- the same core that drove the offscreen/replay hosts now
 * drives a real Linux framebuffer with only the table fill swapped.
 */
#pragma once

#include <cstdint>

#include "host.hpp"  // PixelFormat

namespace cinux::gui {

/**
 * @brief Owning handle on a Linux framebuffer (/dev/fb0)
 *
 * open() maps the framebuffer and reads its geometry; the mapped pixels are the
 * display surface the core flushes into. XRGB8888 (32bpp) is the supported path
 * today; other layouts leave ready()==false.
 */
class FbdevDevice {
public:
    FbdevDevice() = default;
    ~FbdevDevice();

    FbdevDevice(const FbdevDevice&)            = delete;
    FbdevDevice& operator=(const FbdevDevice&) = delete;

    /** Open + mmap @p path (e.g. "/dev/fb0"). Returns false on any failure. */
    bool open(const char* path);

    /** Unmap + close. Idempotent. */
    void close();

    bool     ready() const { return map_ != nullptr; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    uint32_t stride_bytes() const { return stride_; }
    PixelFormat format() const { return format_; }

    /** Base of the mmap'd framebuffer (writable). nullptr if !ready(). */
    void* pixels() { return map_; }

    /**
     * @brief Blit a dirty rect from @p src staging buffer into the framebuffer
     *
     * @p src is the core-owned staging base (XRGB8888); @p src_stride its row
     * pitch. The @p x,@p y,@p w,@p h rect locates the dirty region in display
     * coords; rows are copied with matching offset into the mmap'd framebuffer.
     */
    void blit_rect(int x, int y, int w, int h, const void* src, uint32_t src_stride);

private:
    int         fd_     = -1;
    void*       map_    = nullptr;
    uint32_t    map_len_ = 0;
    uint32_t    width_   = 0;
    uint32_t    height_  = 0;
    uint32_t    stride_  = 0;   // framebuffer row pitch (bytes)
    PixelFormat format_  = PixelFormat::kXrgb8888;
};

}  // namespace cinux::gui
