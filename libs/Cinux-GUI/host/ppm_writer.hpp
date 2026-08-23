/**
 * @file host/ppm_writer.hpp
 * @brief Write an XRGB8888 pixel buffer to a binary PPM (P6) file -- host-side
 *
 * Host utility ONLY: uses fopen/fwrite. The core NEVER writes files (it is
 * host-neutral); dumping a frame to disk is a host/offline concern, used by the
 * offscreen host for golden-frame capture and human inspection.
 *
 * Namespace: cinux::gui
 */
#pragma once

#include <stdint.h>

namespace cinux::gui {

/**
 * @brief Write @p pixels (XRGB8888) to @p path as a binary PPM (P6)
 *
 * @param path         output file path
 * @param width        image width in pixels
 * @param height       image height in pixels
 * @param pixels       base of the XRGB8888 pixel buffer
 * @param stride_bytes row pitch in bytes (may exceed width*4)
 * @return true on success (file opened, header + all pixel bytes written)
 */
bool write_ppm(const char* path, uint32_t width, uint32_t height, const void* pixels,
               uint32_t stride_bytes);

/** Same pixels -> 24-bit BMP (BGR, bottom-up). Viewable in IDEs that preview
 * BMP but not PPM (e.g. VSCode). Returns true on success. */
bool write_bmp(const char* path, uint32_t width, uint32_t height, const void* pixels,
               uint32_t stride_bytes);

}  // namespace cinux::gui
