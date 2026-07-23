/**
 * @file kernel/syscall/sys_iov.cpp
 * @brief sys_writev / sys_readv handler implementations (F10-M1 batch 4)
 *
 * Both walk a user-supplied iovec array and forward each segment to the
 * existing single-buffer sys_write / sys_read, accumulating the byte
 * count.  A segment that returns an error aborts: if nothing was
 * transferred yet the error is propagated, otherwise the partial count is
 * returned (POSIX allows a short writev; musl's stdio loop retries).
 */

#include "kernel/syscall/sys_iov.hpp"

#include <stdint.h>

#include "kernel/errno.hpp"
#include "kernel/syscall/path_util.hpp"
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

}  // anonymous namespace

int64_t sys_writev(uint64_t fd, uint64_t iov_virt, uint64_t iovcnt, uint64_t, uint64_t, uint64_t) {
    if (!validate_user_ptr(iov_virt) || iovcnt == 0 || iovcnt > kMaxIovCnt) {
        return -cinux::kEinval;
    }

    auto*   iov   = reinterpret_cast<const kiovec*>(iov_virt);
    int64_t total = 0;
    for (uint64_t i = 0; i < iovcnt; i++) {
        if (iov[i].iov_len == 0) {
            continue;
        }
        if (!validate_user_ptr(iov[i].iov_base)) {
            return total > 0 ? total : -cinux::kEfault;
        }
        int64_t n = sys_write(fd, iov[i].iov_base, iov[i].iov_len, 0, 0, 0);
        if (n < 0) {
            return total > 0 ? total : n;
        }
        total += n;
        if (static_cast<uint64_t>(n) < iov[i].iov_len) {
            break;  // short write: stop and report what we got
        }
    }
    return total;
}

int64_t sys_readv(uint64_t fd, uint64_t iov_virt, uint64_t iovcnt, uint64_t, uint64_t, uint64_t) {
    if (!validate_user_ptr(iov_virt) || iovcnt == 0 || iovcnt > kMaxIovCnt) {
        return -cinux::kEinval;
    }

    auto*   iov   = reinterpret_cast<const kiovec*>(iov_virt);
    int64_t total = 0;
    for (uint64_t i = 0; i < iovcnt; i++) {
        if (iov[i].iov_len == 0) {
            continue;
        }
        if (!validate_user_ptr(iov[i].iov_base)) {
            return total > 0 ? total : -cinux::kEfault;
        }
        int64_t n = sys_read(fd, iov[i].iov_base, iov[i].iov_len, 0, 0, 0);
        if (n < 0) {
            return total > 0 ? total : n;
        }
        total += n;
        if (static_cast<uint64_t>(n) < iov[i].iov_len) {
            break;  // EOF or short read: stop
        }
    }
    return total;
}

}  // namespace cinux::syscall
