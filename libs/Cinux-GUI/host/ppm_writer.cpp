/**
 * @file host/ppm_writer.cpp
 * @brief Binary PPM (P6) writer for XRGB8888 pixel buffers
 *
 * See ppm_writer.hpp. Each XRGB8888 pixel (0x00RRGGBB) is emitted as 3 bytes
 * R,G,B (the X/alpha byte is dropped). memcpy per pixel avoids aliasing UB and
 * keeps the value-level shift endian-agnostic.
 */

#include "ppm_writer.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace cinux::gui {

bool write_ppm(const char* path, uint32_t width, uint32_t height, const void* pixels,
               uint32_t stride_bytes) {
    if (pixels == nullptr) {
        return false;
    }
    std::FILE* f = std::fopen(path, "wb");
    if (f == nullptr) {
        return false;
    }
    std::fprintf(f, "P6\n%u %u\n255\n", width, height);

    const uint8_t* base = static_cast<const uint8_t*>(pixels);
    bool           ok   = true;
    for (uint32_t y = 0; y < height && ok; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint32_t pv;
            std::memcpy(&pv,
                        base + static_cast<size_t>(y) * stride_bytes + static_cast<size_t>(x) * 4u,
                        4u);
            const uint8_t rgb[3] = {static_cast<uint8_t>((pv >> 16) & 0xFFu),
                                    static_cast<uint8_t>((pv >> 8) & 0xFFu),
                                    static_cast<uint8_t>(pv & 0xFFu)};
            if (std::fwrite(rgb, 1, 3, f) != 3u) {
                ok = false;
                break;
            }
        }
    }
    std::fclose(f);
    return ok;
}

bool write_bmp(const char* path, uint32_t width, uint32_t height, const void* pixels,
               uint32_t stride_bytes) {
    if (pixels == nullptr) {
        return false;
    }
    std::FILE* f = std::fopen(path, "wb");
    if (f == nullptr) {
        return false;
    }
    /* 24-bit BGR, rows padded to 4 bytes, stored bottom-up. */
    const uint32_t row_size  = width * 3u + ((4u - (width * 3u) % 4u) % 4u);
    const uint32_t img_size  = row_size * height;
    const uint32_t file_size = 54u + img_size;
    const auto     put_u16   = [&](uint16_t v) {
        std::fputc(static_cast<int>(v & 0xFFu), f);
        std::fputc(static_cast<int>((v >> 8) & 0xFFu), f);
    };
    const auto put_u32 = [&](uint32_t v) {
        std::fputc(static_cast<int>(v & 0xFFu), f);
        std::fputc(static_cast<int>((v >> 8) & 0xFFu), f);
        std::fputc(static_cast<int>((v >> 16) & 0xFFu), f);
        std::fputc(static_cast<int>((v >> 24) & 0xFFu), f);
    };

    /* BITMAPFILEHEADER (14) */
    std::fputc('B', f);
    std::fputc('M', f);
    put_u32(file_size);
    put_u16(0);
    put_u16(0);
    put_u32(54);
    /* BITMAPINFOHEADER (40) */
    put_u32(40);
    put_u32(width);
    put_u32(height);
    put_u16(1);   // planes
    put_u16(24);  // bpp
    put_u32(0);   // compression (BI_RGB)
    put_u32(img_size);
    put_u32(2835);  // x ppm (~72 dpi)
    put_u32(2835);  // y ppm
    put_u32(0);
    put_u32(0);

    /* Pixels: BGR, bottom-up. */
    const uint8_t* base = static_cast<const uint8_t*>(pixels);
    const uint32_t pad  = row_size - width * 3u;
    bool           ok   = true;
    for (int32_t y = static_cast<int32_t>(height) - 1; y >= 0 && ok; --y) {
        for (uint32_t x = 0; x < width; ++x) {
            uint32_t pv;
            std::memcpy(&pv,
                        base + static_cast<size_t>(y) * stride_bytes + static_cast<size_t>(x) * 4u,
                        4u);
            const uint8_t bgr[3] = {static_cast<uint8_t>(pv & 0xFFu),           // B
                                    static_cast<uint8_t>((pv >> 8) & 0xFFu),    // G
                                    static_cast<uint8_t>((pv >> 16) & 0xFFu)};  // R
            if (std::fwrite(bgr, 1, 3, f) != 3u) {
                ok = false;
                break;
            }
        }
        for (uint32_t p = 0; p < pad; ++p) {
            std::fputc(0, f);
        }
    }
    std::fclose(f);
    return ok;
}

}  // namespace cinux::gui
