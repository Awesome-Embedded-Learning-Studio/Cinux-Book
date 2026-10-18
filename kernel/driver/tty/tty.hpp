/**
 * @file    tty.hpp
 * @brief   The console terminal: kernel-side line editing for fd zero.
 *
 * @author  Charliechen114514
 * @date    2026-10-18
 * @version 0.3.0
 * @since   0.3.0
 * @ingroup kernel_driver
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "cinux/container/ring_queue.hpp"
#include "cinux/singleton.hpp"

namespace cinux::driver {

/// One terminal line's editing budget, NUL included.
inline constexpr unsigned long kTtyLineMax = 128;

/// How many finished lines the terminal keeps before a reader drains it.
inline constexpr unsigned long kTtyPoolBytes = kTtyLineMax * 8;

/**
 * @brief   The terminal on the console pair: line editing in, characters out.
 * @note    The discipline runs inside read, not inside the keyboard
 *          interrupt: fd zero's reader pulls keys one at a time, edits
 *          the line — echo for printable keys, backspace-space-backspace
 *          for erasures — and parks the finished line, newline included,
 *          in a byte pool. A short read leaves the rest of its line in
 *          the pool for the next reader. The write side stays on the
 *          console face; nothing here owns output.
 * @since   0.3.0
 * @ingroup kernel_driver
 */
class Tty : public cinux::base::Singleton<Tty> {
    friend class cinux::base::Singleton<Tty>;

public:
    /**
     * @brief     Hands the reader one edited line, or its first count bytes.
     *
     * @param[out] sink    Where the line's bytes land, newline included.
     * @param[in]  count   How many bytes sink holds.
     * @return    The bytes handed over; blocks until a whole line exists.
     * @note      Keys arrive through the kernel's keyboard take, one per
     *            loop turn, so the editor sleeps between presses and no
     *            interrupt-path code runs under it.
     * @since     0.3.0
     * @ingroup   kernel_driver
     */
    unsigned long read_line(unsigned char* sink, unsigned long count);

    /**
     * @brief     Feeds one key through the editor.
     *
     * @param[in] key   The key as the keyboard translator spelled it.
     * @return    True when this key finished a line.
     * @note      Public because the keyboard face and the reader both
     *            own a piece of the input path; a test world feeds it
     *            directly, the reader feeds it from takes.
     * @since     0.3.0
     * @ingroup   kernel_driver
     */
    bool feed(char key);

    /**
     * @brief     Whether a finished line waits for a reader.
     *
     * @return    True when the pool holds at least one byte.
     * @note      The poll face: readable means read_line hands bytes
     *            over without waiting.
     * @since     0.3.0
     * @ingroup   kernel_driver
     */
    [[nodiscard]] bool has_line() const { return pool_.count() != 0; }

    /**
     * @brief     Whether any input waits anywhere on the input path.
     *
     * @return    True when a key sits at either source seat or a
     *            finished line sits in the pool.
     * @note      The readiness face for ppoll: the discipline runs
     *            inside read_line, so raw keys alone do not make a
     *            line yet — but a reader that goes to read now sets
     *            the discipline working on them, which is the
     *            promise a readiness answer has to keep.
     * @since     0.3.0
     * @ingroup   kernel_driver
     */
    bool has_input();

private:
    Tty() = default;

    /// The line under construction, echoed as it grows.
    char          line_[kTtyLineMax] = {};
    unsigned long used_              = 0;

    /// Finished lines, each newline-terminated, waiting for readers.
    base::container::RingQueue<unsigned char, kTtyPoolBytes + 1> pool_;
};

}  // namespace cinux::driver
