/**
 * @file kernel/ipc/pipe_ops.hpp
 * @brief VFS InodeOps adapters for Pipe reader/writer ends
 *
 * Provides two concrete InodeOps subclasses:
 *   - PipeReadOps:  read() delegates to Pipe::read(), write() returns -1
 *   - PipeWriteOps: write() delegates to Pipe::write(), read() returns -1
 *
 * This split mirrors the UNIX pipe model where each fd represents one
 * direction.  A future pipe() syscall will create a Pipe, then produce
 * two Inodes -- one with PipeReadOps and one with PipeWriteOps.
 *
 * The read/write InodeOps signatures carry an @p offset parameter that
 * pipes ignore -- pipes are byte streams without seek positions.
 *
 * Namespace: cinux::ipc
 */

#pragma once

#include <cstdint>

#include "kernel/fs/inode.hpp"

namespace cinux::ipc {

class Pipe;

// ============================================================
// PipeReadOps -- read-only end of a pipe
// ============================================================

/**
 * @brief InodeOps for the reader end of a Pipe
 *
 * read() delegates to Pipe::read().  write() always returns -1
 * because the reader end is not writable.
 */
class PipeReadOps : public cinux::fs::InodeOps {
public:
    /**
     * @brief Construct a PipeReadOps bound to the given pipe
     * @param pipe     Pointer to the underlying Pipe (must remain valid)
     * @param nonblock If true, reads return WouldBlock (-EAGAIN) instead of
     *                 blocking when the buffer is empty (O_NONBLOCK).
     */
    explicit PipeReadOps(Pipe* pipe, bool nonblock = false);

    /**
     * @brief Read bytes from the pipe
     *
     * @param inode   VFS inode (unused)
     * @param offset  Ignored (pipes are non-seekable)
     * @param buf     Destination buffer
     * @param count   Maximum number of bytes to read
     * @return Number of bytes read, 0 on EOF, or an error (WouldBlock /
     *         InvalidArgument)
     */
    cinux::lib::ErrorOr<int64_t> read(const cinux::fs::Inode* inode, uint64_t offset, void* buf,
                                      uint64_t count) override;

    // F8-M5 poll: read-end readiness delegates to Pipe::poll_read_events.
    uint32_t poll_events(const cinux::fs::Inode* inode, cinux::proc::Task* waiter,
                         bool* registered) override;
    void     poll_detach_waiter(const cinux::fs::Inode* inode, cinux::proc::Task* waiter) override;

    // DEBT-023: last-close hook. FDTable::close -> ~File -> InodeOps::release
    // (once every fd pointing at this read inode is gone) drops one read-end
    // reference on the shared Pipe; on the final one it fires EOF.
    void release(cinux::fs::Inode* inode) override;

private:
    Pipe* pipe_;
    bool  nonblock_;
};

// ============================================================
// PipeWriteOps -- write-only end of a pipe
// ============================================================

/**
 * @brief InodeOps for the writer end of a Pipe
 *
 * write() delegates to Pipe::write().  read() always returns -1
 * because the writer end is not readable.
 */
class PipeWriteOps : public cinux::fs::InodeOps {
public:
    /**
     * @brief Construct a PipeWriteOps bound to the given pipe
     * @param pipe     Pointer to the underlying Pipe (must remain valid)
     * @param nonblock If true, writes return WouldBlock (-EAGAIN) instead of
     *                 blocking when the buffer is full (O_NONBLOCK).
     */
    explicit PipeWriteOps(Pipe* pipe, bool nonblock = false);

    /**
     * @brief Write bytes to the pipe
     *
     * @param inode   VFS inode (unused)
     * @param offset  Ignored (pipes are non-seekable)
     * @param buf     Source buffer
     * @param count   Number of bytes to write
     * @return Number of bytes written, or an error (WouldBlock / BrokenPipe /
     *         InvalidArgument)
     */
    cinux::lib::ErrorOr<int64_t> write(cinux::fs::Inode* inode, uint64_t offset, const void* buf,
                                       uint64_t count) override;

    // F8-M5 poll: write-end readiness delegates to Pipe::poll_write_events.
    uint32_t poll_events(const cinux::fs::Inode* inode, cinux::proc::Task* waiter,
                         bool* registered) override;
    void     poll_detach_waiter(const cinux::fs::Inode* inode, cinux::proc::Task* waiter) override;

    // DEBT-023: last-close hook (see PipeReadOps::release). Drops one write-end
    // reference on the shared Pipe; on the final one it raises BrokenPipe.
    void release(cinux::fs::Inode* inode) override;

private:
    Pipe* pipe_;
    bool  nonblock_;
};

}  // namespace cinux::ipc
