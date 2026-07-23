/**
 * @file kernel/ipc/pipe_ops.cpp
 * @brief PipeReadOps and PipeWriteOps implementations
 *
 * Thin VFS adapters that delegate to the underlying Pipe object.
 * The offset parameter from InodeOps is intentionally ignored because
 * pipes are non-seekable byte streams.  PIPE_WOULDBLOCK (from a non-blocking
 * call on a full/empty pipe) is translated to Error::WouldBlock (-EAGAIN);
 * a write to a reader-less pipe is translated to Error::BrokenPipe (-EPIPE,
 * which sys_write turns into SIGPIPE).
 */

#include "kernel/ipc/pipe_ops.hpp"

#include "kernel/ipc/pipe.hpp"

namespace cinux::ipc {

// ============================================================
// PipeReadOps
// ============================================================

PipeReadOps::PipeReadOps(Pipe* pipe, bool nonblock) : pipe_(pipe), nonblock_(nonblock) {}

cinux::lib::ErrorOr<int64_t> PipeReadOps::read(const cinux::fs::Inode*, uint64_t, void* buf, uint64_t count) {
    if (pipe_ == nullptr || buf == nullptr) {
        return cinux::lib::Error::InvalidArgument;
    }
    int64_t n = pipe_->read(static_cast<char*>(buf), count, nonblock_);
    if (n == PIPE_WOULDBLOCK) {
        return cinux::lib::Error::WouldBlock;
    }
    if (n >= 0) {
        return n;  // byte count, or 0 for EOF
    }
    return cinux::lib::Error::IOError;  // n == -1: invalid argument
}

// ============================================================
// PipeWriteOps
// ============================================================

PipeWriteOps::PipeWriteOps(Pipe* pipe, bool nonblock) : pipe_(pipe), nonblock_(nonblock) {}

cinux::lib::ErrorOr<int64_t> PipeWriteOps::write(cinux::fs::Inode*, uint64_t, const void* buf, uint64_t count) {
    if (pipe_ == nullptr || buf == nullptr) {
        return cinux::lib::Error::InvalidArgument;
    }
    int64_t n = pipe_->write(static_cast<const char*>(buf), count, nonblock_);
    if (n == PIPE_WOULDBLOCK) {
        return cinux::lib::Error::WouldBlock;
    }
    if (n >= 0) {
        return n;
    }
    // n < 0: reader gone -> BrokenPipe (-EPIPE, sys_write raises SIGPIPE); else InvalidArgument.
    return pipe_->reader_alive() ? cinux::lib::Error::InvalidArgument
                                 : cinux::lib::Error::BrokenPipe;
}

}  // namespace cinux::ipc
