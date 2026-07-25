/**
 * @file kernel/drivers/tty/pty.hpp
 * @brief Pseudoterminal (PTY) master/slave pair (F10-M3 Phase 2)
 *
 * A PTY connects a "terminal emulator" (the master side) to an application's
 * controlling terminal (the slave side) through a bidirectional byte pipe that
 * runs the slave's input through a line discipline.  It is the substrate a real
 * shell runs under: the emulator types on the master, the shell reads cooked
 * lines from the slave and writes output back to the master.
 *
 * Data flow:
 *   - master_write()  : each byte feeds the slave's TTY line discipline
 *                       (canonical editing / echo / signal chars).  Echo lands
 *                       on the master read side so the emulator sees typing.
 *   - slave_read()    : drains the cooked line the discipline committed
 *                       (raw bytes in ~ICANON).
 *   - slave_write()   : program output -> master read side.
 *   - master_read()   : drains whatever the slave produced (output + echo).
 *
 * The slave owns a TTY (reused from Phase 1, not re-implemented); its echo sink
 * is wired to push bytes back to the master, so local echo routes through the
 * pair exactly as on a real terminal.  Signal chars typed on the master surface
 * as TtySignal via take_pending_signal(); the kernel wiring (Phase 2 batch 4)
 * delivers them to the slave's foreground process group.
 *
 * Pure logic -- no kprintf, no proc, no Console -- so the unit links clean in
 * the host test harness exactly like tty.cpp.  Blocking (wait-for-data) is a
 * scheduling concern left to the kernel side; these primitives are non-blocking
 * and return the byte count moved (0 when nothing is ready).
 *
 * Namespace: cinux::drivers
 */

#pragma once
#include <cinux/ring_buffer.hpp>

#include <stddef.h>
#include <stdint.h>

#include <cinux/expected.hpp>

#include "kernel/drivers/tty/tty.hpp"

namespace cinux::drivers {

/// A pseudoterminal pair.  One TTY line discipline on the slave side; a ring
/// buffer on the master read side collecting slave output + echo.
class Pty {
public:
    /// Capacity of the master read ring (slave output + echo waiting for the
    /// emulator to read).  Matches the TTY cooked queue order of magnitude.
    static constexpr size_t kMasterBufSize = 4096;

    Pty();

    /// Type into the terminal: each byte feeds the slave line discipline
    /// (editing, echo, signal processing).  Returns the count consumed.
    cinux::lib::ErrorOr<int64_t> master_write(const void* buf, uint64_t count);

    /// Drain what the slave produced (program output + local echo).  Returns
    /// the byte count moved; 0 when nothing is pending.
    cinux::lib::ErrorOr<int64_t> master_read(void* buf, uint64_t count);

    /// Application read: cooked input (canonical line / raw bytes).  Returns
    /// the byte count; 0 when no line is ready (caller blocks or checks EOF).
    cinux::lib::ErrorOr<int64_t> slave_read(void* buf, uint64_t count);

    /// Application write: program output routed to the master read side.
    /// Returns the count accepted (may be < count if the master ring fills).
    cinux::lib::ErrorOr<int64_t> slave_write(const void* buf, uint64_t count);

    /// The slave's line discipline (for TCGETS/TCSETS/TIOCGWINSZ and EOF probe
    /// via the slave fd).
    TTY&       slave_tty();
    const TTY& slave_tty() const;

    /// Reset to a freshly-constructed state (clear the master ring and give the
    /// slave a clean line discipline).  Used when a PTY table slot is reused --
    /// re-constructing in place keeps the echo sink anchored to this object (a
    /// plain `Pty p{};` move-assignment would dangle the echo context at the
    /// temporary).
    void reset();

    /// Signal requested by a slave line-discipline char (^C/^\/^Z typed on the
    /// master).  pending_signal() peeks; take_pending_signal() clears it.
    TtySignal pending_signal() const;
    TtySignal take_pending_signal();

private:
    // Echo sink target: the slave TTY echoes typed chars back to the master.
    static void echo_thunk(char c, void* ctx);
    void        echo_to_master(char c);

    // Master read ring (head/tail/full, same shape as TTY's cooked queue).
    bool   master_push(char c);
    size_t master_pop(char* buf, size_t maxlen);

    TTY slave_tty_;

    cinux::lib::RingBuffer<char, kMasterBufSize> master_buf_;
};

}  // namespace cinux::drivers
