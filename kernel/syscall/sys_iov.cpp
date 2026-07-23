/**
 * @file kernel/syscall/sys_iov.cpp
 * @brief sys_writev / sys_readv handler implementations (F10-M1 batch 4 / P0b)
 *
 * Walks a user-supplied iovec array and forwards each segment to the
 * single-buffer sys_write / sys_read (which are already SMAP-layered
 * boundaries: they stage the user segment via copy_from/to_user and call
 * do_write/do_read_kernel). A segment that errors aborts: if nothing was
 * transferred yet the error is propagated, otherwise the partial count is
 * returned (POSIX allows a short writev; musl's stdio loop retries).
 *
 * P0b (SMAP): the iovec array itself is user memory. It is read in chunks of
 * kIovChunk via copy_from_user (never a raw dereference), and each segment's
 * user base pointer is handed to sys_write/sys_read, whose own boundary does
 * the per-segment accessor staging. Chunked (8 at a time) so a bogus iovcnt
 * can't blow the kernel stack.
 */

#include "kernel/syscall/sys_iov.hpp"

#include <stdint.h>

#include "kernel/arch/x86_64/user_access.hpp"  // P0b (SMAP): access_ok / copy_from_user
#include "kernel/errno.hpp"
#include "kernel/syscall/sys_read.hpp"
#include "kernel/syscall/sys_write.hpp"

namespace cinux::syscall {

namespace {

/// Linux struct iovec layout: { void *iov_base; size_t iov_len; }.
struct kiovec {
    uint64_t iov_base;
    uint64_t iov_len;
};

/// Bounds cap so a bogus iovcnt can't drive a huge loop.
constexpr uint64_t kMaxIovCnt = 1024;
/// Chunk size for the rolling copy_from_user of the iovec array (stack-safe).
constexpr uint64_t kIovChunk  = 8;

}  // anonymous namespace

int64_t sys_writev(uint64_t fd, uint64_t iov_virt, uint64_t iovcnt, uint64_t, uint64_t, uint64_t) {
    if (iovcnt == 0 || iovcnt > kMaxIovCnt) {
        return -cinux::kEinval;
    }
    if (!cinux::user::access_ok(reinterpret_cast<void*>(iov_virt), iovcnt * sizeof(kiovec))) {
        return -cinux::kEinval;
    }

    int64_t total = 0;
    for (uint64_t off = 0; off < iovcnt; off += kIovChunk) {
        uint64_t n = (iovcnt - off < kIovChunk) ? iovcnt - off : kIovChunk;
        kiovec   kiov[kIovChunk];
        if (!cinux::user::copy_from_user(kiov,
                                         reinterpret_cast<void*>(iov_virt + off * sizeof(kiovec)),
                                         n * sizeof(kiovec))) {
            return total > 0 ? total : -cinux::kEfault;
        }
        for (uint64_t i = 0; i < n; i++) {
            if (kiov[i].iov_len == 0) {
                continue;
            }
            // sys_write is the per-segment user boundary: it access_ok's +
            // stages iov_base, then do_write_kernel. iov_base stays a user
            // pointer here; only sys_write dereferences it (via accessor).
            int64_t wr = sys_write(fd, kiov[i].iov_base, kiov[i].iov_len, 0, 0, 0);
            if (wr < 0) {
                return total > 0 ? total : wr;
            }
            total += wr;
            if (static_cast<uint64_t>(wr) < kiov[i].iov_len) {
                return total;  // short write: stop and report what we got
            }
        }
    }
    return total;
}

int64_t sys_readv(uint64_t fd, uint64_t iov_virt, uint64_t iovcnt, uint64_t, uint64_t, uint64_t) {
    if (iovcnt == 0 || iovcnt > kMaxIovCnt) {
        return -cinux::kEinval;
    }
    if (!cinux::user::access_ok(reinterpret_cast<void*>(iov_virt), iovcnt * sizeof(kiovec))) {
        return -cinux::kEinval;
    }

    int64_t total = 0;
    for (uint64_t off = 0; off < iovcnt; off += kIovChunk) {
        uint64_t n = (iovcnt - off < kIovChunk) ? iovcnt - off : kIovChunk;
        kiovec   kiov[kIovChunk];
        if (!cinux::user::copy_from_user(kiov,
                                         reinterpret_cast<void*>(iov_virt + off * sizeof(kiovec)),
                                         n * sizeof(kiovec))) {
            return total > 0 ? total : -cinux::kEfault;
        }
        for (uint64_t i = 0; i < n; i++) {
            if (kiov[i].iov_len == 0) {
                continue;
            }
            int64_t rd = sys_read(fd, kiov[i].iov_base, kiov[i].iov_len, 0, 0, 0);
            if (rd < 0) {
                return total > 0 ? total : rd;
            }
            total += rd;
            if (static_cast<uint64_t>(rd) < kiov[i].iov_len) {
                return total;  // EOF or short read: stop
            }
        }
    }
    return total;
}

}  // namespace cinux::syscall
