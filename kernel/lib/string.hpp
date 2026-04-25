/**
 * @file kernel/lib/string.hpp
 * @brief Freestanding string/memory utility declarations
 *
 * Provides basic memory and string operations for the kernel, replacing
 * the libc functions that are unavailable in a freestanding environment.
 *
 * Namespace: global (C linkage, matching libc signatures)
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

extern "C" {

/**
 * @brief Fill memory with a byte value
 */
void* memset(void* dest, int val, size_t count);

/**
 * @brief Copy memory (src and dest must not overlap)
 */
void* memcpy(void* __restrict__ dest, const void* __restrict__ src, size_t count);

/**
 * @brief Copy memory (handles overlapping regions)
 */
void* memmove(void* dest, const void* src, size_t count);

/**
 * @brief Compare two memory regions
 */
int memcmp(const void* a, const void* b, size_t count);

/**
 * @brief Compare two null-terminated strings
 */
int strcmp(const char* a, const char* b);

/**
 * @brief Compare two strings up to n characters
 */
int strncmp(const char* a, const char* b, size_t n);

/**
 * @brief Compute the length of a null-terminated string
 */
size_t strlen(const char* s);

/**
 * @brief Copy a null-terminated string from src to dest
 *
 * dest must have enough space. Returns dest.
 */
char* strcpy(char* __restrict__ dest, const char* __restrict__ src);

/**
 * @brief Convert an unsigned integer to a null-terminated string
 *
 * Writes the decimal representation of @p value into @p buf.
 * @p buf must have space for at least 11 bytes (uint32_t max = 10 digits + NUL).
 *
 * @param buf    Output buffer
 * @param value  Value to convert
 * @return       Number of characters written (excluding NUL)
 */
int utoa(char* buf, uint32_t value);

}  // extern "C"
