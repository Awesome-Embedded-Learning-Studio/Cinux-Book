/**
 * @file    tty.hpp
 * @brief   A terminal line editor: keys in, edited lines out, one instance
 *          per terminal.
 *
 * @author  Charliechen114514
 * @date    2026-10-19
 * @version 0.5.0
 * @since   0.3.0
 * @ingroup kernel_driver
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "cinux/container/ring_queue.hpp"
#include "kernel/driver/tty/tty_config.hpp"

namespace cinux::driver {

/**
 * @brief   The three seats one terminal plugs into, plus the context they
 *          share.
 * @note    A Tty owns the discipline, not the plumbing: where keys come
 *          from, where echo goes, and how a waiting reader parks are the
 *          assembler's decisions, handed over once at wiring. The seats
 *          are captureless functions with one context word — the same
 *          pair-shape FsOps uses — so one discipline class serves many
 *          terminals: the console wires serial-plus-keyboard, console
 *          echo and the scheduler's yield; a PTY slave wires its own
 *          master's byte rings both ways through the context; a test
 *          world wires fakes.
 * @since   0.5.0
 * @ingroup kernel_driver
 */
struct TtyWiring {
    /// The word every seat receives back, unchecked.
    void* context = nullptr;

    /// Pulls one input byte when one waits; false when none does.
    bool (*take_key)(void* context, char* out) = nullptr;

    /// Whether the input seat holds a byte right now, non-destructive.
    bool (*has_key)(void* context) = nullptr;

    /// Where the discipline's own echo characters go.
    void (*echo)(void* context, char key) = nullptr;

    /// How a reader with no input yet lets the rest of the world run.
    void (*wait)(void* context) = nullptr;
};

/**
 * @brief   One terminal's line discipline, instance-shaped.
 * @note    The discipline runs inside read, not inside an interrupt: the
 *          reader pulls bytes one at a time, edits the line under the
 *          termios flags that govern it — echo for printable keys when
 *          ECHO stands, backspace-space-backspace for erasures, raw
 *          pass-through when ICANON falls — and parks finished lines,
 *          newline included, in a byte pool. A short read leaves the
 *          rest of its line in the pool for the next reader. The write
 *          side never belonged here; a terminal owns input editing.
 * @since   0.3.0
 * @ingroup kernel_driver
 */
class Tty {
public:
    /**
     * @brief     Builds an unwired terminal, born quiet.
     *
     * @return    None
     * @note      A constant-initializable shape: the kernel's own
     *              terminals sit in function-local constinit storage,
     *              so construction takes no runtime step. Wire() hands
     *              over the seats and the governing flags in one call
     *              a discipline refuses to work without.
     * @since     0.5.0
     * @ingroup   kernel_driver
     */
    Tty() = default;

    /**
     * @brief     Builds and wires one terminal to its three seats.
     *
     * @param[in] wiring   The input, echo and wait seats.
     * @return    None
     * @note      The wiring is borrowed for the instance's whole life;
     *              it typically points at static functions.
     * @since     0.5.0
     * @ingroup   kernel_driver
     */
    explicit Tty(const TtyWiring& wiring);

    /**
     * @brief     Hands over the seats and the born flags.
     *
     * @param[in] wiring   The input, echo and wait seats.
     * @return    None
     * @note      Call once before any discipline work; it installs the
     *              wiring and the fully-canonical born flags. Calling
     *              it again would undo a caller's termios, so the
     *              assembler asks Wired() first.
     * @since     0.5.0
     * @ingroup   kernel_driver
     */
    void wire(const TtyWiring& wiring);

    /**
     * @brief     Whether the seats are in place.
     *
     * @return    True after Wire, false before.
     * @since     0.5.0
     * @ingroup   kernel_driver
     */
    [[nodiscard]] bool wired() const { return wiring_ != nullptr; }

    /**
     * @brief     Hands the reader one edited line, or its first count bytes.
     *
     * @param[out] sink    Where the line's bytes land, newline included.
     * @param[in]  count   How many bytes sink holds.
     * @return    The bytes handed over; blocks until a whole line exists
     *              in canonical mode, until any byte exists otherwise.
     * @since     0.3.0
     * @ingroup   kernel_driver
     */
    unsigned long read_line(unsigned char* sink, unsigned long count);

    /**
     * @brief     Feeds one byte through the discipline.
     *
     * @param[in] key   The byte as the input seat spelled it.
     * @return    True when this byte finished a line.
     * @note      Public because the input seat and the reader each own a
     *              piece of the path; a test world feeds it directly, the
     *              reader feeds it from takes.
     * @since     0.3.0
     * @ingroup   kernel_driver
     */
    bool feed(char key);

    /**
     * @brief     Whether a finished line waits for a reader.
     *
     * @return    True when the pool holds at least one byte.
     * @since     0.3.0
     * @ingroup   kernel_driver
     */
    [[nodiscard]] bool has_line() const { return pool_.count() != 0; }

    /**
     * @brief     Whether any input waits anywhere on the input path.
     *
     * @return    True when a byte sits at the seat, a line is under
     *              construction, or a finished line sits in the pool.
     * @note      The readiness face for poll: the discipline runs inside
     *              read, so raw bytes alone do not make a line yet — but
     *              a reader that goes to read now sets the discipline
     *              working on them, which is the promise a readiness
     *              answer has to keep. In non-canonical mode the seat's
     *              own byte is already deliverable.
     * @since     0.3.0
     * @ingroup   kernel_driver
     */
    [[nodiscard]] bool has_input() const;

    /**
     * @brief     Copies the governing termios record out, 60 hosted bytes.
     *
     * @param[out] sink   Where the record lands; kTermiosBytes wide.
     * @return    None
     * @note      The record answers back exactly what was last applied,
     *              with the three governing flags — ECHO, ICANON, ISIG —
     *              standing or falling by their stored bits.
     * @since     0.5.0
     * @ingroup   kernel_driver
     */
    void copy_termios(unsigned char* sink) const;

    /**
     * @brief     Takes a termios record in as the governing one.
     *
     * @param[in] source   The record to adopt; kTermiosBytes wide.
     * @return    None
     * @note      The whole record is stored and answers back verbatim;
     *              of its flags, ECHO gates echo, ICANON gates line
     *              editing, and ISIG recognizes VINTR, VQUIT and VSUSP
     *              independently of ICANON.
     * @since     0.5.0
     * @ingroup   kernel_driver
     */
    void apply_termios(const unsigned char* source);

    /**
     * @brief     The terminal's window shape, rows then columns.
     *
     * @return    The winsize record values this instance reports.
     * @since     0.5.0
     * @ingroup   kernel_driver
     */
    [[nodiscard]] TtyWindow window() const { return window_; }

    /**
     * @brief     Sets the window shape ioctl answers report.
     *
     * @param[in] window   Rows and columns to report from now on.
     * @return    None
     * @since     0.5.0
     * @ingroup   kernel_driver
     */
    void set_window(TtyWindow window) { window_ = window; }

    /**
     * @brief     Takes the signal a key asked for, if one waits.
     *
     * @return    The signal number, zero when none waits: 2 for ^C,
     *            3 for ^\, 20 for ^Z, the hosted numbers.
     * @note      With ISIG standing the discipline eats those keys and
     *            parks the number here; the reader that pulls the keys
     *            also takes the number and delivers it. Input still
     *            requires a reader to run the discipline.
     * @since     0.5.0
     * @ingroup   kernel_driver
     */
    int take_pending_signal();

private:
    /// Halts loudly when the seats are missing.
    void require_wired() const;

    /// Echoes one character when ECHO stands.
    void echo_char(char key) const;

    /// Pushes the finished line into the pool and clears the buffer.
    void park_line();

    /**
     * @brief     Maps an enabled signal character to its signal.
     * @param[in] key   The byte to compare against the termios controls.
     * @return    Signal number, or zero for ordinary or disabled input.
     * @since     0.5.0
     * @ingroup   kernel_driver
     */
    [[nodiscard]] int signal_for(char key) const;

    /// The parked signal number, zero when none.
    int pending_signal_ = 0;

    const TtyWiring* wiring_ = nullptr;

    /// The line under construction, echoed as it grows.
    char          line_[kTtyLineMax] = {};
    unsigned long used_              = 0;

    /// Finished lines, each newline-terminated, waiting for readers.
    base::container::RingQueue<unsigned char, kTtyPoolBytes + 1> pool_;

    /// The governing termios record, host-shaped, born fully canonical.
    unsigned char termios_[kTermiosBytes] = {};

    /// The window shape this terminal reports.
    TtyWindow window_{};
};

}  // namespace cinux::driver
