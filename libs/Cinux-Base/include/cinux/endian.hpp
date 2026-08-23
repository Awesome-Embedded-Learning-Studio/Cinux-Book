/**
 * @file endian.hpp
 * @brief Byte-order conversion utilities — bswap, htobe/htole, POSIX aliases.
 *
 * Uses compiler predefined macros (__BYTE_ORDER__) for compile-time detection.
 * All functions are constexpr.
 */

#ifndef CINUX_ENDIAN_HPP
#define CINUX_ENDIAN_HPP

#include <cstdint>

// Undefine glibc macros that clash with our function names.
// glibc <endian.h> defines htobe16/htole32/etc. as preprocessor macros,
// which would mangle our namespace-qualified declarations.
#ifdef bswap16
#    undef bswap16
#endif
#ifdef bswap32
#    undef bswap32
#endif
#ifdef bswap64
#    undef bswap64
#endif
#ifdef htobe16
#    undef htobe16
#endif
#ifdef htobe32
#    undef htobe32
#endif
#ifdef htobe64
#    undef htobe64
#endif
#ifdef betoh16
#    undef betoh16
#endif
#ifdef betoh32
#    undef betoh32
#endif
#ifdef betoh64
#    undef betoh64
#endif
#ifdef htole16
#    undef htole16
#endif
#ifdef htole32
#    undef htole32
#endif
#ifdef htole64
#    undef htole64
#endif
#ifdef letoh16
#    undef letoh16
#endif
#ifdef letoh32
#    undef letoh32
#endif
#ifdef letoh64
#    undef letoh64
#endif
#ifdef ntohs
#    undef ntohs
#endif
#ifdef ntohl
#    undef ntohl
#endif
#ifdef htons
#    undef htons
#endif
#ifdef htonl
#    undef htonl
#endif

namespace cinux::lib {

// ============================================================
// Host endianness detection
// ============================================================

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#    define CINUX_LITTLE_ENDIAN 1
#elif defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#    define CINUX_LITTLE_ENDIAN 0
#else
// Default assumption: x86_64 / ARM little-endian
#    define CINUX_LITTLE_ENDIAN 1
#endif

constexpr bool is_little_endian() {
    return CINUX_LITTLE_ENDIAN;
}
constexpr bool is_big_endian() {
    return !CINUX_LITTLE_ENDIAN;
}

// ============================================================
// Byte swap
// ============================================================

constexpr uint16_t bswap16(uint16_t v) {
    return static_cast<uint16_t>((v >> 8) | (v << 8));
}

constexpr uint32_t bswap32(uint32_t v) {
    return ((v >> 24) & 0x000000FFu) | ((v >> 8) & 0x0000FF00u) | ((v << 8) & 0x00FF0000u) |
           ((v << 24) & 0xFF000000u);
}

constexpr uint64_t bswap64(uint64_t v) {
    return ((v >> 56) & 0x00000000000000FFULL) | ((v >> 40) & 0x000000000000FF00ULL) |
           ((v >> 24) & 0x0000000000FF0000ULL) | ((v >> 8) & 0x00000000FF000000ULL) |
           ((v << 8) & 0x000000FF00000000ULL) | ((v << 24) & 0x0000FF0000000000ULL) |
           ((v << 40) & 0x00FF000000000000ULL) | ((v << 56) & 0xFF00000000000000ULL);
}

// ============================================================
// Host <-> Big-Endian
// ============================================================

constexpr uint16_t htobe16(uint16_t v) {
    return CINUX_LITTLE_ENDIAN ? bswap16(v) : v;
}
constexpr uint32_t htobe32(uint32_t v) {
    return CINUX_LITTLE_ENDIAN ? bswap32(v) : v;
}
constexpr uint64_t htobe64(uint64_t v) {
    return CINUX_LITTLE_ENDIAN ? bswap64(v) : v;
}

constexpr uint16_t betoh16(uint16_t v) {
    return CINUX_LITTLE_ENDIAN ? bswap16(v) : v;
}
constexpr uint32_t betoh32(uint32_t v) {
    return CINUX_LITTLE_ENDIAN ? bswap32(v) : v;
}
constexpr uint64_t betoh64(uint64_t v) {
    return CINUX_LITTLE_ENDIAN ? bswap64(v) : v;
}

// ============================================================
// Host <-> Little-Endian
// ============================================================

constexpr uint16_t htole16(uint16_t v) {
    return CINUX_LITTLE_ENDIAN ? v : bswap16(v);
}
constexpr uint32_t htole32(uint32_t v) {
    return CINUX_LITTLE_ENDIAN ? v : bswap32(v);
}
constexpr uint64_t htole64(uint64_t v) {
    return CINUX_LITTLE_ENDIAN ? v : bswap64(v);
}

constexpr uint16_t letoh16(uint16_t v) {
    return CINUX_LITTLE_ENDIAN ? v : bswap16(v);
}
constexpr uint32_t letoh32(uint32_t v) {
    return CINUX_LITTLE_ENDIAN ? v : bswap32(v);
}
constexpr uint64_t letoh64(uint64_t v) {
    return CINUX_LITTLE_ENDIAN ? v : bswap64(v);
}

// ============================================================
// POSIX-style aliases (network byte order)
// ============================================================

constexpr uint16_t ntohs(uint16_t v) {
    return betoh16(v);
}
constexpr uint32_t ntohl(uint32_t v) {
    return betoh32(v);
}
constexpr uint16_t htons(uint16_t v) {
    return htobe16(v);
}
constexpr uint32_t htonl(uint32_t v) {
    return htobe32(v);
}

}  // namespace cinux::lib

#endif  // CINUX_ENDIAN_HPP
