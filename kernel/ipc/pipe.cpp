/**
 * @file kernel/ipc/pipe.cpp
 * @brief Pipe implementation -- ring-buffer read/write with scheduler blocking
 *
 * F8-M1: the old sti/hlt spin loop is gone.  Blocking now uses the scheduler's
 * prepare_to_wait()/schedule_blocked()/unblock() triplet (the same pattern as
 * Mutex::lock and the console TTY): a full/empty buffer parks the caller on a
 * wait queue, and the peer wakes it after draining/enqueuing or on close.
 * This is #DF-safe (no sti inside the syscall path).  The host unit-test build
 * has no scheduler, so CINUX_HOST_TEST compiles the blocking path out and
 * reads/writes return what fits -- synchronous tests never hit full/empty.
 */

#include "kernel/ipc/pipe.hpp"

#include <stddef.h>
#include <stdint.h>

#ifndef CINUX_HOST_TEST
#    include "kernel/proc/process.hpp"    // Task::wait_next
#    include "kernel/proc/scheduler.hpp"  // prepare_to_wait/schedule_blocked/unblock
#endif

namespace cinux::ipc {

#ifndef CINUX_HOST_TEST
namespace {
using cinux::proc::Scheduler;
using cinux::proc::Task;

/// Append @p t to the tail of a wait queue (intrusive via Task::wait_next).
void wait_enqueue(Task*& head, Task* t) {
    t->wait_next = nullptr;
    if (head == nullptr) {
        head = t;
        return;
    }
    Task* tail = head;
    while (tail->wait_next != nullptr) {
        tail = tail->wait_next;
    }
    tail->wait_next = t;
}

/// Remove and return the queue head, or nullptr if empty.
Task* wait_dequeue(Task*& head) {
    Task* t = head;
    if (t != nullptr) {
        head         = t->wait_next;
        t->wait_next = nullptr;
    }
    return t;
}

/// Wake one waiter (FIFO head).  Called under lock_; this is safe because,
/// unlike a mutex, the pipe hands no ownership to the woken task -- it simply
/// re-acquires the pipe lock fresh when the scheduler runs it (after we drop
/// the lock).  No AB-BA: the scheduler run-queue lock is never held across a
/// pipe-lock acquisition.
void wake_one(Task*& head) {
    Task* t = wait_dequeue(head);
    if (t != nullptr) {
        Scheduler::unblock(t);
    }
}

/// Wake every waiter (used on close so blocked peers observe the close).
void wake_all(Task*& head) {
    while (Task* t = wait_dequeue(head)) {
        Scheduler::unblock(t);
    }
}
}  // namespace
#endif  // CINUX_HOST_TEST

// ============================================================
// Constructor
// ============================================================

Pipe::Pipe() : reader_open_(true), writer_open_(true) {}

// ============================================================
// Write
// ============================================================

int64_t Pipe::write(const char* data, uint64_t count, bool nonblock) {
    if (data == nullptr || count == 0) {
        return -1;
    }

    uint64_t written = 0;
    for (;;) {
#ifndef CINUX_HOST_TEST
        bool need_block = false;
#endif
        {
            auto g = lock_.irq_guard();

            // Reader closed: no point buffering more.  The caller maps -1 to
            // BrokenPipe (-> SIGPIPE); any bytes already pushed stay readable.
            if (!reader_open_) {
                break;
            }

            // Push as many bytes as fit; push_batch handles wrap-around.
            if (!buf_.full()) {
                size_t space  = PIPE_BUFFER_SIZE - buf_.size();
                size_t remain = static_cast<size_t>(count - written);
                size_t chunk  = remain < space ? remain : space;
                written += buf_.push_batch(data + written, chunk);
#ifndef CINUX_HOST_TEST
                // Newly buffered data may unblock a reader.
                wake_one(read_waiters_);
#endif
            }

            if (written >= count) {
                break;  // whole request satisfied
            }

            // Still bytes to write but the buffer is full.
            if (nonblock) {
                return written > 0 ? static_cast<int64_t>(written) : PIPE_WOULDBLOCK;
            }
#ifdef CINUX_HOST_TEST
            break;  // host: no scheduler, never block; return partial
#else
            // Blocking: park on the write wait queue.  prepare_to_wait() under
            // the irq_guard makes the Blocked flip atomic vs a concurrent reader
            // draining (no lost wakeup); schedule_blocked() runs after the guard
            // drops, with IRQs restored and the lock released.
            Task* self = Scheduler::current();
            if (self == nullptr) {
                break;  // no scheduler context (early boot): don't block
            }
            wait_enqueue(write_waiters_, self);
            Scheduler::prepare_to_wait(self);
            need_block = true;
#endif
        }  // irq_guard drops: IRQs restored + lock released BEFORE switching out
#ifndef CINUX_HOST_TEST
        if (need_block) {
            Scheduler::schedule_blocked();
        }
        // Woken by a reader freeing space (or by close_reader); loop and retry.
#endif
    }

    if (written > 0) {
        return static_cast<int64_t>(written);
    }
    if (!reader_open_) {
        return -1;  // reader gone -> BrokenPipe
    }
    return 0;  // host early-break with nothing pushed, reader still open
}

// ============================================================
// Read
// ============================================================

int64_t Pipe::read(char* buf, uint64_t count, bool nonblock) {
    if (buf == nullptr || count == 0) {
        return -1;
    }

    uint64_t total_read = 0;
    for (;;) {
#ifndef CINUX_HOST_TEST
        bool need_block = false;
#endif
        {
            auto g = lock_.irq_guard();

            // Drain whatever is buffered; pop_batch handles wrap-around.
            if (!buf_.empty()) {
                size_t avail  = buf_.size();
                size_t remain = static_cast<size_t>(count - total_read);
                size_t chunk  = remain < avail ? remain : avail;
                total_read += buf_.pop_batch(buf + total_read, chunk);
#ifndef CINUX_HOST_TEST
                // Freed space may unblock a writer.
                wake_one(write_waiters_);
#endif
            }

            if (total_read >= count) {
                break;  // satisfied the full request
            }

            // Writer closed: return what we have, or EOF if nothing was read.
            if (!writer_open_) {
                return total_read > 0 ? static_cast<int64_t>(total_read) : 0;
            }

            // Writer still open but nothing (more) buffered right now.
            if (nonblock) {
                return total_read > 0 ? static_cast<int64_t>(total_read) : PIPE_WOULDBLOCK;
            }
#ifdef CINUX_HOST_TEST
            break;  // host: no scheduler, never block; return partial
#else
            Task* self = Scheduler::current();
            if (self == nullptr) {
                break;
            }
            wait_enqueue(read_waiters_, self);
            Scheduler::prepare_to_wait(self);
            need_block = true;
#endif
        }
#ifndef CINUX_HOST_TEST
        if (need_block) {
            Scheduler::schedule_blocked();
        }
#endif
    }

    return total_read > 0 ? static_cast<int64_t>(total_read) : 0;
}

// ============================================================
// Close endpoints
// ============================================================

void Pipe::close_reader() {
    auto g       = lock_.irq_guard();
    reader_open_ = false;
#ifndef CINUX_HOST_TEST
    // Wake all writers so they retry, see reader_open_ == false, and return -1
    // (BrokenPipe -> SIGPIPE).
    wake_all(write_waiters_);
#endif
}

void Pipe::close_writer() {
    auto g       = lock_.irq_guard();
    writer_open_ = false;
#ifndef CINUX_HOST_TEST
    // Wake all readers so they retry, see writer_open_ == false, and return EOF
    // (or the remaining buffered bytes).
    wake_all(read_waiters_);
#endif
}

// ============================================================
// State queries (lock-free -- diagnostics / fast-path checks)
// ============================================================

bool Pipe::reader_alive() const {
    return reader_open_;
}

bool Pipe::writer_alive() const {
    return writer_open_;
}

bool Pipe::is_empty() const {
    return buf_.empty();
}

bool Pipe::is_full() const {
    return buf_.full();
}

uint32_t Pipe::available() const {
    return static_cast<uint32_t>(buf_.size());
}

// ============================================================
// Non-blocking try_read / try_write (ignore wait queues)
// ============================================================

int64_t Pipe::try_read(char* buf, uint64_t count) {
    if (buf == nullptr || count == 0) {
        return -1;
    }

    auto guard = lock_.guard();

    // Writer closed and buffer drained -- EOF
    if (!writer_open_ && buf_.empty()) {
        return 0;
    }

    // Buffer empty -- return 0 immediately (no spin-wait)
    if (buf_.empty()) {
        return 0;
    }

    size_t avail  = buf_.size();
    size_t want   = static_cast<size_t>(count);
    size_t chunk  = want < avail ? want : avail;
    size_t popped = buf_.pop_batch(buf, chunk);
    return static_cast<int64_t>(popped);
}

int64_t Pipe::try_write(const char* data, uint64_t count) {
    if (data == nullptr || count == 0) {
        return -1;
    }

    auto guard = lock_.guard();

    // Reader has closed -- cannot write
    if (!reader_open_) {
        return -1;
    }

    // Buffer full -- return 0 immediately (no spin-wait)
    if (buf_.full()) {
        return 0;
    }

    size_t space  = PIPE_BUFFER_SIZE - buf_.size();
    size_t want   = static_cast<size_t>(count);
    size_t chunk  = want < space ? want : space;
    size_t pushed = buf_.push_batch(data, chunk);
    return static_cast<int64_t>(pushed);
}

}  // namespace cinux::ipc
