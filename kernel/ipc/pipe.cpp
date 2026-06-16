/**
 * @file kernel/ipc/pipe.cpp
 * @brief Pipe implementation -- ring-buffer read/write with spin-wait blocking
 */

#include "kernel/ipc/pipe.hpp"

#include <stddef.h>
#include <stdint.h>

#include "kernel/arch/x86_64/irq.hpp"

namespace cinux::ipc {

using cinux::arch::irq_disable;
using cinux::arch::irq_enable;
using cinux::arch::irq_save;
using cinux::arch::irq_restore;
using cinux::arch::hlt;

// ============================================================
// Constructor
// ============================================================

Pipe::Pipe() : reader_open_(true), writer_open_(true) {}

// ============================================================
// Write
// ============================================================

int64_t Pipe::write(const char* data, uint64_t count) {
    if (data == nullptr || count == 0) {
        return -1;
    }

    // Disable interrupts while the lock is held to prevent deadlock with
    // PIT's try_read() -- the IRQ handler runs on the same CPU and would
    // spin forever trying to acquire the same lock.
    uint64_t orig_flags = irq_save();

    uint64_t written = 0;

    while (written < count) {
        lock_.acquire();

        if (!reader_open_) {
            lock_.release();
            goto out;
        }

        // Spin-wait if the buffer is completely full.
        // Release the lock AND re-enable interrupts so the reader (PIT)
        // can drain the pipe and make progress.
        if (buf_.full()) {
            lock_.release();
            irq_enable();

            for (uint32_t i = 0; i < PIPE_SPIN_WAIT_ITERS; i++) {
                hlt();
                irq_disable();

                lock_.acquire();
                bool still_full  = buf_.full();
                bool reader_gone = !reader_open_;
                lock_.release();

                if (!still_full || reader_gone) {
                    break;
                }
                irq_enable();
            }
            irq_disable();
            continue;
        }

        // Push as many bytes as fit; push_batch handles wrap-around.
        size_t space  = PIPE_BUFFER_SIZE - buf_.size();
        size_t remain = static_cast<size_t>(count - written);
        size_t chunk  = remain < space ? remain : space;
        written += buf_.push_batch(data + written, chunk);

        lock_.release();
    }

out:
    irq_restore(orig_flags);
    return (written > 0) ? static_cast<int64_t>(written) : (reader_open_ ? 0 : -1);
}

// ============================================================
// Read
// ============================================================

int64_t Pipe::read(char* buf, uint64_t count) {
    if (buf == nullptr || count == 0) {
        return -1;
    }

    // Same IRQ-safety rationale as write().
    uint64_t orig_flags = irq_save();

    uint64_t total_read = 0;

    while (total_read < count) {
        lock_.acquire();

        // Writer closed and buffer drained -- EOF
        if (!writer_open_ && buf_.empty()) {
            lock_.release();
            goto out;
        }

        // Spin-wait if the buffer is empty.
        if (buf_.empty()) {
            lock_.release();
            irq_enable();

            for (uint32_t i = 0; i < PIPE_SPIN_WAIT_ITERS; i++) {
                hlt();
                irq_disable();

                lock_.acquire();
                bool still_empty = buf_.empty();
                bool writer_gone = !writer_open_;
                lock_.release();

                if (!still_empty || writer_gone) {
                    break;
                }
                irq_enable();
            }
            irq_disable();
            continue;
        }

        // Pop as many bytes as available; pop_batch handles wrap-around.
        size_t avail  = buf_.size();
        size_t remain = static_cast<size_t>(count - total_read);
        size_t chunk  = remain < avail ? remain : avail;
        total_read += buf_.pop_batch(buf + total_read, chunk);

        lock_.release();
    }

out:
    irq_restore(orig_flags);
    return (total_read > 0) ? static_cast<int64_t>(total_read) : (writer_open_ ? 0 : 0);
}

// ============================================================
// Close endpoints
// ============================================================

void Pipe::close_reader() {
    auto guard   = lock_.guard();
    reader_open_ = false;
}

void Pipe::close_writer() {
    auto guard   = lock_.guard();
    writer_open_ = false;
}

// ============================================================
// State queries (lock-free -- for diagnostics / fast-path checks)
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
// Non-blocking try_read / try_write
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
