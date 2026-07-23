/**
 * @file kernel/errno.hpp
 * @brief Canonical POSIX errno constants and Error->errno translation.
 *
 * Syscall handlers consume @c cinux::lib::ErrorOr internally, but user space
 * observes the standard Linux convention: a negative return value carries the
 * negated errno (@c -ENOENT, @c -EACCES, ...). This header owns that
 * translation boundary.
 *
 * Freestanding -- it does not pull in libc @c <errno.h>; the constants below
 * mirror the Linux numeric values and carry the @c k prefix per CODING-TASTE.
 */

#pragma once

#include <cinux/expected.hpp>  // cinux::lib::Error
#include <cstdint>

namespace cinux {

/// POSIX errno values used at the syscall boundary.
constexpr int kEperm        = 1;    ///< Operation not permitted
constexpr int kEnoent       = 2;    ///< No such file or directory
constexpr int kEsrch        = 3;    ///< No such process
constexpr int kEio          = 5;    ///< I/O error
constexpr int kEbadf        = 9;    ///< Bad file descriptor
constexpr int kEagain       = 11;   ///< Resource temporarily unavailable
constexpr int kEnomem       = 12;   ///< Cannot allocate memory
constexpr int kEacces       = 13;   ///< Permission denied
constexpr int kEfault       = 14;   ///< Bad address
constexpr int kEbusy        = 16;   ///< Device or resource busy
constexpr int kEexist       = 17;   ///< File exists
constexpr int kEnotdir      = 20;   ///< Not a directory
constexpr int kEisdir       = 21;   ///< Is a directory
constexpr int kEinval       = 22;   ///< Invalid argument
constexpr int kEmfile       = 24;   ///< Too many open files
constexpr int kEnotty       = 25;   ///< Inappropriate ioctl for device
constexpr int kEnospc       = 28;   ///< No space left on device
constexpr int kEpipe        = 32;   ///< Broken pipe
constexpr int kErange       = 34;   ///< Numerical result out of range (buffer too small)
constexpr int kEnametoolong = 36;   ///< File name too long
constexpr int kEnosys       = 38;   ///< Function not implemented
constexpr int kEnotempty    = 39;   ///< Directory not empty
constexpr int kEtimedout    = 110;  ///< Connection timed out
constexpr int kEconnrefused = 111;  ///< Connection refused

/**
 * @brief Map a @c cinux::lib::Error to its POSIX errno value.
 *
 * Used by syscall handlers to convert internal @c ErrorOr failures into the
 * negative errno convention visible to user space. Always returns a
 * non-negative errno; callers negate it before returning from a handler.
 */
constexpr int to_errno(cinux::lib::Error e) {
    switch (e) {
    case cinux::lib::Error::Ok:
        return 0;
    case cinux::lib::Error::NotFound:
        return kEnoent;
    case cinux::lib::Error::PermissionDenied:
        return kEacces;
    case cinux::lib::Error::OutOfMemory:
        return kEnomem;
    case cinux::lib::Error::IOError:
        return kEio;
    case cinux::lib::Error::AlreadyExists:
        return kEexist;
    case cinux::lib::Error::InvalidArgument:
        return kEinval;
    case cinux::lib::Error::WouldBlock:
        return kEagain;
    case cinux::lib::Error::BrokenPipe:
        return kEpipe;
    case cinux::lib::Error::BufferOverflow:
        return kEnametoolong;
    case cinux::lib::Error::NotImplemented:
        return kEnosys;
    case cinux::lib::Error::Busy:
        return kEbusy;
    case cinux::lib::Error::ConnectionRefused:
        return kEconnrefused;
    case cinux::lib::Error::TimedOut:
        return kEtimedout;
    }
    return kEio;  // unmapped Error falls back to a generic I/O failure
}

}  // namespace cinux
