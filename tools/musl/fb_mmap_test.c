/*
 * fb_mmap_test.c — ring-3 smoke for /dev/fb0 mmap (F-GUI-USERSPACE batch 1b).
 *
 * Opens /dev/fb0, queries geometry via ioctl(FBIOGET_SCREENINFO), mmaps the
 * framebuffer, writes a pixel, reads it back, and exits 0.  This is the
 * end-to-end exercise of the batch-1a IoPhys VMA path: sys_mmap -> fb_dev
 * mmap hook -> IoPhys VMA -> handle_pf IoPhys branch -> uncached device page.
 *
 * Built static against the Cinux musl sysroot (see build-fb-mmap-test.sh).
 */
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/mman.h>

/* Mirror of kernel/drivers/video/fb_dev.hpp kFbioGetScreenInfo + FbScreenInfo. */
#define FBIOGET_SCREENINFO 0x4600
struct fb_screen_info {
    uint32_t width;
    uint32_t height;
    uint32_t pitch; /* bytes per scan line */
    uint32_t bpp;   /* bits per pixel */
};

int main(void) {
    int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) {
        return 1;
    }

    struct fb_screen_info info;
    if (ioctl(fd, FBIOGET_SCREENINFO, &info) != 0) {
        return 2;
    }
    if (info.width == 0 || info.height == 0 || info.pitch == 0 || info.bpp == 0) {
        return 3; /* ioctl returned, but geometry is nonsense */
    }

    size_t   sz = (size_t)info.pitch * (size_t)info.height;
    uint8_t* fb = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fb == (uint8_t*)-1) {
        return 4; /* mmap failed -- the device-mmap hook or IoPhys VMA setup broke */
    }

    /* Write a red pixel at (0,0).  This is the crux: the first access faults
     * the IoPhys VMA, and the page-fault handler must map the real VBE
     * framebuffer physical page (FLAG_PCD, uncached) -- NOT allocate a RAM
     * page.  A read-back mismatch would mean the kernel backed the mapping
     * with anonymous memory instead of device memory. */
    *(volatile uint32_t*)fb = 0x00FF0000u;
    if (*(volatile uint32_t*)fb != 0x00FF0000u) {
        return 5;
    }

    /* Touch a page near the end too, to exercise the offset math
     * (phys_base + (page - start)) on a non-first page. */
    size_t last_off                      = sz - (size_t)info.pitch;
    *(volatile uint32_t*)(fb + last_off) = 0x0000FF00u; /* green at last row */

    return 0;
}
