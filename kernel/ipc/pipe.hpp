/**
 * @file kernel/ipc/pipe.hpp
 * @brief Byte-stream pipe with a 4 KB ring buffer for IPC
 *
 * Pipe provides a unidirectional, kernel-internal byte channel between
 * a writer and a reader.  It is the building block for anonymous pipes
 * exposed through the VFS (see PipeReadOps / PipeWriteOps).
 *
 * Blocking semantics (F8-M1): when the buffer is full (write) or empty
 * (read), the caller truly blocks on a scheduler wait queue -- it is woken
 * when the peer makes progress (reader drains / writer enqueues) or when the
 * peer end closes.  This replaces the old sti/hlt spin loop, which was a
 * #DF hazard inside the syscall path (sti-in-syscall -> LAPIC tick traps the
 * kernel stack -> sysretq corrupts).  The blocking path mirrors the proven
 * prepare_to_wait()/schedule_blocked()/unblock() pattern used by Mutex and the
 * console TTY.  In the host unit-test build there is no scheduler, so the
 * blocking path is compiled out (CINUX_HOST_TEST) and reads/writes simply
 * return what fits -- host tests are synchronous and never hit a full/empty
 * condition.  O_NONBLOCK callers pass nonblock=true and receive kWouldBlock
 * instead of blocking.
 *
 * The backing store is cinux::lib::RingBuffer (Cinux-Base); the ring buffer
 * itself is not thread-safe, so all access is guarded by lock_ (irq_guard --
 * IRQs off closes the lost-wakeup window vs the peer on another CPU).
 *
 * Namespace: cinux::ipc
 */

#pragma once

#include <stdint.h>

#include <cinux/ring_buffer.hpp>

#include "kernel/fs/inode.hpp"  // kPoll* event bits (poll readiness helpers)
#include "kernel/proc/sync.hpp"

namespace cinux::proc {
struct Task;  // forward -- wait queues hold blocked readers/writers
}

namespace cinux::ipc {

// ============================================================
// Constants
// ============================================================

/// Ring buffer size in bytes (one page)
static constexpr uint32_t PIPE_BUFFER_SIZE = 4096;

/// Sentinel returned by read()/write() when a non-blocking call would block.
/// The InodeOps adapters translate it to Error::WouldBlock -> -EAGAIN.
static constexpr int64_t PIPE_WOULDBLOCK = -2;

// ============================================================
// Pipe -- unidirectional byte stream
// ============================================================

/**
 * @brief Unidirectional byte-stream pipe backed by a ring buffer
 *
 * A Pipe connects writers and readers.  Bytes written are buffered in a 4 KB
 * circular buffer and consumed in FIFO order by readers.  Closing the writer
 * signals EOF to readers (read returns 0 once drained); closing the reader
 * fails further writes (write returns -1, mapped to BrokenPipe/SIGPIPE).
 */
class Pipe {
public:
    Pipe();

    // -- Non-copyable, non-movable --
    Pipe(const Pipe&)            = delete;
    Pipe& operator=(const Pipe&) = delete;

    // -- Public interface --------------------------------------------------

    /**
     * @brief Write bytes into the pipe
     *
     * Pushes up to @p count bytes from @p data.  If the buffer fills, blocks on
     * the write wait queue until a reader frees space or the reader closes.
     *
     * @param data      Source buffer
     * @param count     Number of bytes to write
     * @param nonblock  If true, never block: return PIPE_WOULDBLOCK when the
     *                  buffer is full and nothing was written
     * @return Bytes written (>= 0); -1 if the reader closed (BrokenPipe);
     *         PIPE_WOULDBLOCK if @p nonblock and the buffer is full
     */
    int64_t write(const char* data, uint64_t count, bool nonblock = false);

    /**
     * @brief Read bytes from the pipe
     *
     * Pops up to @p count bytes into @p buf.  If the buffer is empty, blocks on
     * the read wait queue until a writer enqueues data or the writer closes
     * (EOF -> 0).
     *
     * @param buf       Destination buffer
     * @param count     Maximum number of bytes to read
     * @param nonblock  If true, never block: return PIPE_WOULDBLOCK when the
     *                  buffer is empty and the writer is still open
     * @return Bytes read (>= 0); 0 on EOF; -1 on invalid argument;
     *         PIPE_WOULDBLOCK if @p nonblock and the buffer is empty
     */
    int64_t read(char* buf, uint64_t count, bool nonblock = false);

    /**
     * @brief Close the reader end
     *
     * After this, reader_alive() is false.  Blocked writers are woken so they
     * observe the close and return -1 (BrokenPipe).
     */
    void close_reader();

    /**
     * @brief Close the writer end
     *
     * After this, writer_alive() is false.  Blocked readers are woken so they
     * observe EOF once the buffer drains.
     */
    void close_writer();

    // -- Open-description refcount (DEBT-023) -----------------------------
    // Each PipeReadOps / PipeWriteOps inode -- one per pipe() end, or per FIFO
    // open() direction (FIFO clones a fresh end inode) -- holds one ref on the
    // matching end.  Reaching 0 = the LAST referring fd closed -> emit EOF /
    // BrokenPipe to the peer.  Without this a dup (or a 2nd FIFO open) trips an
    // early EOF on its first close, breaking `ls|grep`.  Bumped in the ops ctor,
    // dropped from InodeOps::release (the File's last close).  close_reader /
    // close_writer above remain for direct (non-fd) users (GUI terminal, tests).
    void add_read_ref();
    void add_write_ref();
    void release_read_ref();
    void release_write_ref();

    /** @brief True if the reader end has not been closed. */
    bool reader_alive() const;

    /** @brief True if the writer end has not been closed. */
    bool writer_alive() const;

    /** @brief True if no bytes are buffered. */
    bool is_empty() const;

    /** @brief True if the buffer is completely full. */
    bool is_full() const;

    /** @brief Number of bytes currently available to read. */
    uint32_t available() const;

    // -- poll(2) / select(2) readiness (F8-M5) -----------------------------
    // These register a poller on this pipe's wait queue (atomically with the
    // readiness check, under lock_) so sys_poll can block until the peer makes
    // progress.  The poller follows with remove_*_waiter() after it wakes.

    /**
     * @brief Read-end readiness for poll/select.
     *
     * Returns @c kPollIn if bytes are buffered and @c kPollHup if the writer
     * end has closed (Linux reports both when data remains after close).  If
     * @p waiter is non-null, also enqueues it on the read wait queue so a later
     * write/close wakes it.
     */
    uint32_t poll_read_events(cinux::proc::Task* waiter);

    /**
     * @brief Write-end readiness for poll/select.
     *
     * Returns @c kPollOut if the buffer has space and @c kPollErr if the reader
     * end has closed (further writes would raise SIGPIPE).  If @p waiter is
     * non-null, also enqueues it on the write wait queue so a later drain/close
     * wakes it.
     */
    uint32_t poll_write_events(cinux::proc::Task* waiter);

    /** Remove a poll waiter from the read wait queue (no-op if not queued). */
    void remove_read_waiter(cinux::proc::Task* waiter);

    /** Remove a poll waiter from the write wait queue (no-op if not queued). */
    void remove_write_waiter(cinux::proc::Task* waiter);

    /**
     * @brief Non-blocking read (ignores wait queues)
     *
     * Reads up to @p count bytes; returns 0 immediately if empty (EOF if the
     * writer also closed).  Does not touch the scheduler, so it is safe where
     * blocking is impossible (e.g. IRQ-driven consumers).
     */
    int64_t try_read(char* buf, uint64_t count);

    /**
     * @brief Non-blocking write (ignores wait queues)
     *
     * Writes up to @p count bytes; returns 0 immediately if full, -1 if the
     * reader is closed.  Does not touch the scheduler.
     */
    int64_t try_write(const char* data, uint64_t count);

private:
    // -- Ring buffer state -------------------------------------------------
    cinux::lib::RingBuffer<char, PIPE_BUFFER_SIZE> buf_;

    // -- Endpoint state ----------------------------------------------------
    bool reader_open_;
    bool writer_open_;

    // Open-description refcounts per end (DEBT-023). Guarded by lock_.
    uint32_t read_refs_{0};
    uint32_t write_refs_{0};

    // -- Synchronisation ---------------------------------------------------
    /// Protects buf_, the open flags, and the wait queues; irq_guard closes the
    /// lost-wakeup window vs a peer on another CPU (prepare_to_wait is done
    /// under this guard, matching Mutex::lock).
    cinux::proc::Spinlock lock_;

    /// Tasks blocked in read() (buffer was empty).  Intrusive list via
    /// Task::wait_next.
    cinux::proc::Task* read_waiters_{nullptr};
    /// Tasks blocked in write() (buffer was full).
    cinux::proc::Task* write_waiters_{nullptr};
};

}  // namespace cinux::ipc
