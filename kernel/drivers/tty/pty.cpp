/**
 * @file kernel/drivers/tty/pty.cpp
 * @brief Pseudoterminal pair implementation (F10-M3 Phase 2)
 *
 * Pure logic -- no kprintf, no proc, no Console.  The slave's echo is routed to
 * the master read ring through the TTY's echo sink, so the unit links clean in
 * the host test harness.  See pty.hpp for the data-flow design.
 */

#include "kernel/drivers/tty/pty.hpp"

namespace cinux::drivers {

Pty::Pty() : slave_tty_() {
    // Echo typed input back to the master: the emulator sees what it typed,
    // exactly as local echo on a real terminal would.
    slave_tty_.set_echo_sink(&Pty::echo_thunk, this);
}

// ============================================================
// Echo routing (slave TTY -> master read ring)
// ============================================================

void Pty::echo_thunk(char c, void* ctx) {
    static_cast<Pty*>(ctx)->echo_to_master(c);
}

void Pty::echo_to_master(char c) {
    // Echo is best-effort: if the master never drains and the ring fills,
    // drop the echoed byte rather than block the input path.
    (void)master_push(c);
}

// ============================================================
// Master read ring
// ============================================================

bool Pty::master_push(char c) {
    if (master_full_) {
        return false;
    }
    master_buf_[master_tail_] = c;
    master_tail_              = (master_tail_ + 1) % kMasterBufSize;
    if (master_tail_ == master_head_) {
        master_full_ = true;
    }
    return true;
}

size_t Pty::master_pop(char* buf, size_t maxlen) {
    size_t n = 0;
    while (n < maxlen) {
        if (master_head_ == master_tail_ && !master_full_) {
            break;  // empty
        }
        buf[n++]     = master_buf_[master_head_];
        master_head_ = (master_head_ + 1) % kMasterBufSize;
        master_full_ = false;
    }
    return n;
}

// ============================================================
// Master side (terminal emulator)
// ============================================================

cinux::lib::ErrorOr<int64_t> Pty::master_write(const void* buf, uint64_t count) {
    if (buf == nullptr && count > 0) {
        return cinux::lib::Error::InvalidArgument;
    }
    const auto* p = static_cast<const char*>(buf);
    for (uint64_t i = 0; i < count; ++i) {
        // Each byte drives canonical editing, echo (-> master), and signal
        // processing inside the discipline; the return value is ignored here
        // (signals surface via take_pending_signal, overflow drops the byte).
        (void)slave_tty_.input_char(p[i]);
    }
    return static_cast<int64_t>(count);
}

cinux::lib::ErrorOr<int64_t> Pty::master_read(void* buf, uint64_t count) {
    if (buf == nullptr && count > 0) {
        return cinux::lib::Error::InvalidArgument;
    }
    return static_cast<int64_t>(master_pop(static_cast<char*>(buf), static_cast<size_t>(count)));
}

// ============================================================
// Slave side (application)
// ============================================================

cinux::lib::ErrorOr<int64_t> Pty::slave_read(void* buf, uint64_t count) {
    if (buf == nullptr && count > 0) {
        return cinux::lib::Error::InvalidArgument;
    }
    // Drains the cooked line the discipline committed (raw bytes in ~ICANON).
    // EOF is a separate signal: the caller checks slave_tty().take_eof() when
    // this returns 0, so "no line yet" stays distinct from real EOF.
    return static_cast<int64_t>(
        slave_tty_.read_cooked(static_cast<char*>(buf), static_cast<size_t>(count)));
}

cinux::lib::ErrorOr<int64_t> Pty::slave_write(const void* buf, uint64_t count) {
    if (buf == nullptr && count > 0) {
        return cinux::lib::Error::InvalidArgument;
    }
    const auto* p = static_cast<const char*>(buf);
    uint64_t    n = 0;
    for (uint64_t i = 0; i < count; ++i) {
        // Partial write when the master ring fills: the kernel side retries the
        // remainder once the emulator drains (mirrors a blocking slave write).
        if (!master_push(p[i])) {
            break;
        }
        ++n;
    }
    return static_cast<int64_t>(n);
}

// ============================================================
// Line-discipline access + signal forwarding
// ============================================================

TTY& Pty::slave_tty() {
    return slave_tty_;
}
const TTY& Pty::slave_tty() const {
    return slave_tty_;
}

void Pty::reset() {
    master_head_ = 0;
    master_tail_ = 0;
    master_full_ = false;
    // Fresh line discipline (default termios, empty line/cooked buffers).
    slave_tty_   = TTY{};
    // The echo sink must keep routing to this object: re-wire it after the
    // re-assignment above (which left the new TTY's echo callback null).
    slave_tty_.set_echo_sink(&Pty::echo_thunk, this);
}

TtySignal Pty::pending_signal() const {
    return slave_tty_.pending_signal();
}

TtySignal Pty::take_pending_signal() {
    return slave_tty_.take_signal();
}

}  // namespace cinux::drivers
