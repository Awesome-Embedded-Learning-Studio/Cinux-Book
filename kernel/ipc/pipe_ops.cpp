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
#include "kernel/errno.hpp"  // kEintr

namespace cinux::ipc {

namespace {
// Sentinel success value meaning "interrupted by signal" -- a real byte count
// is always >= 0, so -1 is unused.  do_read_kernel / sys_write detect it and
// return -EINTR.  (We cannot add a variant to lib::Error without touching the
// Cinux-Base submodule, so the same convention the socket recv path uses
// applies here too.)
constexpr int64_t kEintrSentinel = -1;
}  // namespace

// ============================================================
// PipeReadOps
// ============================================================

PipeReadOps::PipeReadOps(Pipe* pipe, bool nonblock) : pipe_(pipe), nonblock_(nonblock) {
    if (pipe_ != nullptr) {
        pipe_->add_read_ref();  // DEBT-023: this read inode accounts for one end
    }
}

cinux::lib::ErrorOr<int64_t> PipeReadOps::read(const cinux::fs::Inode*, uint64_t, void* buf,
                                               uint64_t count) {
    if (pipe_ == nullptr || buf == nullptr) {
        return cinux::lib::Error::InvalidArgument;
    }
    int64_t n = pipe_->read(static_cast<char*>(buf), count, nonblock_);
    if (n == PIPE_WOULDBLOCK) {
        return cinux::lib::Error::WouldBlock;
    }
    // EINTR from a signal-interrupted blocking read: forward as the sentinel
    // success value so do_read_kernel can map it to -EINTR.
    if (n == -cinux::kEintr) {
        return kEintrSentinel;
    }
    if (n >= 0) {
        return n;  // byte count, or 0 for EOF
    }
    return cinux::lib::Error::IOError;  // n == -1: invalid argument
}

uint32_t PipeReadOps::poll_events(const cinux::fs::Inode*, cinux::proc::Task* waiter,
                                  bool* registered) {
    // The poller is registered iff it asked to be parked (waiter != null); a
    // pipe is a blocking fd type, so poll may sleep on it.
    if (registered != nullptr) {
        *registered = (waiter != nullptr);
    }
    return pipe_->poll_read_events(waiter);
}

void PipeReadOps::poll_detach_waiter(const cinux::fs::Inode*, cinux::proc::Task* waiter) {
    pipe_->remove_read_waiter(waiter);
}

void PipeReadOps::release(cinux::fs::Inode*) {
    if (pipe_ != nullptr) {
        pipe_->release_read_ref();  // DEBT-023: last fd on this read end -> EOF
    }
}

// ============================================================
// PipeWriteOps
// ============================================================

PipeWriteOps::PipeWriteOps(Pipe* pipe, bool nonblock) : pipe_(pipe), nonblock_(nonblock) {
    if (pipe_ != nullptr) {
        pipe_->add_write_ref();  // DEBT-023: this write inode accounts for one end
    }
}

cinux::lib::ErrorOr<int64_t> PipeWriteOps::write(cinux::fs::Inode*, uint64_t, const void* buf,
                                                 uint64_t count) {
    if (pipe_ == nullptr || buf == nullptr) {
        return cinux::lib::Error::InvalidArgument;
    }
    int64_t n = pipe_->write(static_cast<const char*>(buf), count, nonblock_);
    if (n == PIPE_WOULDBLOCK) {
        return cinux::lib::Error::WouldBlock;
    }
    // EINTR from a signal-interrupted blocking write: forward as the sentinel.
    if (n == -cinux::kEintr) {
        return kEintrSentinel;
    }
    if (n >= 0) {
        return n;
    }
    // n < 0: distinguish a closed reader (BrokenPipe) from an invalid argument.
    // When the reader is gone, Pipe::write returns -1 and reader_alive() is
    // false.  sys_write maps BrokenPipe -> -EPIPE and raises SIGPIPE -- the
    // whole point of returning BrokenPipe here rather than a generic IOError
    // (which maps to -EIO and never triggers SIGPIPE).
    return pipe_->reader_alive() ? cinux::lib::Error::InvalidArgument
                                 : cinux::lib::Error::BrokenPipe;
}

uint32_t PipeWriteOps::poll_events(const cinux::fs::Inode*, cinux::proc::Task* waiter,
                                   bool* registered) {
    if (registered != nullptr) {
        *registered = (waiter != nullptr);
    }
    return pipe_->poll_write_events(waiter);
}

void PipeWriteOps::poll_detach_waiter(const cinux::fs::Inode*, cinux::proc::Task* waiter) {
    pipe_->remove_write_waiter(waiter);
}

void PipeWriteOps::release(cinux::fs::Inode*) {
    if (pipe_ != nullptr) {
        pipe_->release_write_ref();  // DEBT-023: last fd on this write end -> BrokenPipe
    }
}

}  // namespace cinux::ipc
