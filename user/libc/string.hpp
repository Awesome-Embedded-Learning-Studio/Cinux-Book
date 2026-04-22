/**
 * @file user/libc/string.hpp
 * @brief Freestanding string / memory utility functions for user-space programs
 *
 * Provides the familiar C library string / memory primitives so that
 * user programs can manipulate buffers without linking against libstdc++.
 * All functions live in namespace cinux::user.
 *
 * Dependencies: <cstddef>, <cstdint> (freestanding headers).
 */

#pragma once

#include <cstddef>

namespace cinux::user {

/**
 * @brief Compute the length of a NUL-terminated string
 *
 * @param s Pointer to the string
 * @return Number of characters before the terminating NUL
 */
size_t strlen(const char* s);

/**
 * @brief Compare two NUL-terminated strings lexicographically
 *
 * @param a First string
 * @param b Second string
 * @return 0 if equal, negative if a < b, positive if a > b
 */
int strcmp(const char* a, const char* b);

/**
 * @brief Fill a memory region with a byte value
 *
 * @param dest Pointer to the region
 * @param c    Byte value to fill with
 * @param n    Number of bytes to fill
 * @return Pointer to @p dest
 */
void* memset(void* dest, int c, size_t n);

/**
 * @brief Copy bytes from source to destination (no overlap guarantee)
 *
 * @param dest Destination buffer
 * @param src  Source buffer
 * @param n    Number of bytes to copy
 * @return Pointer to @p dest
 */
void* memcpy(void* dest, const void* src, size_t n);

/**
 * @brief Compare two memory regions byte-by-byte
 *
 * @param a First region
 * @param b Second region
 * @param n Number of bytes to compare
 * @return 0 if equal, negative if first differing byte in a < b, positive otherwise
 */
int memcmp(const void* a, const void* b, size_t n);

}  // namespace cinux::user
