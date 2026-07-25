/**
 * @file kernel/syscall/sys_setsockopt.cpp
 * @brief sys_setsockopt handler (F-ECO batch 7a)
 *
 * See sys_setsockopt.hpp.  The option payload (@p optval) is deliberately NOT
 * read -- no option is honoured, so there is nothing to copy in.  This also
 * means the ring0 test kernel can exercise it (no copy_from_user on a user ptr;
 * the test passes a kernel addr that we ignore).
 *
 * Namespace: cinux::syscall
 */

#include "kernel/syscall/sys_setsockopt.hpp"

#include <stdint.h>

#include "kernel/errno.hpp"
#include "kernel/syscall/sys_socket.hpp"  // socket_from_fd

namespace cinux::syscall {

int64_t sys_setsockopt(uint64_t fd, uint64_t /*level*/, uint64_t /*optname*/, uint64_t /*optval*/,
                       uint64_t /*optlen*/, uint64_t) {
    if (socket_from_fd(fd) == nullptr) {
        return -cinux::kEbadf;
    }
    // No socket-option storage: accept every option as a no-op.  Documented
    // hobby-OS simplification (SO_REUSEADDR / RCVBUF / KEEPALIVE / NODELAY all
    // succeed but have no effect); faithful option semantics is a follow-up.
    return 0;
}

}  // namespace cinux::syscall
