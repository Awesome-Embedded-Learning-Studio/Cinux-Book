/**
 * @file    pty.hpp
 * @brief   One pseudo-terminal pair: a master's pipes around a real
 *          line discipline.
 *
 * A PTY is not two pipes: the slave side is a genuine terminal. This
 * class holds one pair — the slave is a wired Tty whose keys arrive from
 * the master's writes and whose echo flows back to the master's read
 * side, while the slave's own writes land in the same master ring. Four
 * paths cross the pair: master writes feed the discipline, the slave's
 * readers take edited lines, the slave's writers push output, and the
 * master's readers drain everything the slave produced. The discipline
 * flags travel with the slave: a termios applied to the slave governs
 * its editing exactly as the console's governs fd zero.
 *
 * @author  Charliechen114514
 * @date    2026-10-19
 * @version 0.1
 * @since   0.5.0
 * @ingroup kernel_driver
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <atomic>

#include "cinux/container/ring_queue.hpp"
#include "kernel/driver/tty/pty_config.hpp"
#include "kernel/driver/tty/tty.hpp"

namespace cinux::driver {

/**
 * @brief   One master/slave terminal pair, discipline included.
 * @note    Wiring is the pair's own business: the slave's Tty takes its
 *          keys from this pair's raw ring and echoes into this pair's
 *          master ring, whatever a constructor could not express with
 *          captureless seats it receives as the wait hook. The rings are
 *          byte queues with no reader blocking inside this class. The
 *          devfs faces return WouldBlock while a peer is live; the
 *          syscall layer chooses between waiting and EAGAIN. Independent
 *          opens pin each end; dup and fork share those pins through
 *          OpenFile. The last master close hangs up the slave discipline.
 * @since   0.5.0
 * @ingroup kernel_driver
 */
class Pty {
public:
    /**
     * @brief     Builds the pair around one wait hook.
     *
     * @param[in] wait   How a blocked side lets the world run.
     * @return    None
     * @note      The hook is typically the scheduler's yield; a test
     *              world hands a no-op. The pair wires itself: the
     *              slave's seats point back here, so a Pty must live
     *              where it was built — copies and moves are refused.
     * @since     0.5.0
     * @ingroup   kernel_driver
     */
    explicit Pty(void (*wait)(void* context));

    Pty(const Pty&)            = delete;
    Pty& operator=(const Pty&) = delete;
    Pty(Pty&&)                 = delete;
    Pty& operator=(Pty&&)      = delete;

    /**
     * @brief     The slave side's terminal, the real discipline.
     *
     * @return    The pair's Tty, wired to this pair's rings.
     * @since     0.5.0
     * @ingroup   kernel_driver
     */
    Tty& slave();

    /**
     * @brief     Feeds bytes into the pair as if typed at the master.
     *
     * @param[in] source   Bytes the master end writes.
     * @param[in] count    How many.
     * @return    Bytes taken into the raw ring; a full ring takes what
     *              fits and refuses the rest.
     * @note      The discipline consumes on the slave's next read; echo
     *              of those keys lands in the master ring then, not here.
     * @since     0.5.0
     * @ingroup   kernel_driver
     */
    unsigned long master_write(const void* source, unsigned long count);

    /**
     * @brief     Takes the pair's output, echo and slave writes alike.
     *
     * @param[out] sink    Where the master's bytes land.
     * @param[in]  count   How many sink holds.
     * @return    Bytes handed over, zero when the ring is empty.
     * @since     0.5.0
     * @ingroup   kernel_driver
     */
    unsigned long master_read(void* sink, unsigned long count);

    /**
     * @brief     Pushes the slave program's own output to the master.
     *
     * @param[in] source   Bytes the slave side writes.
     * @param[in] count    How many.
     * @return    Bytes landed in the master ring; a full ring takes what
     *              fits and refuses the rest.
     * @since     0.5.0
     * @ingroup   kernel_driver
     */
    unsigned long slave_write(const void* source, unsigned long count);

    /**
     * @brief     Whether the master can read output or observe slave EOF.
     *
     * @return    True when the master's reader would make progress.
     * @since     0.5.0
     * @ingroup   kernel_driver
     */
    [[nodiscard]] bool master_has_input() const;

    /**
     * @brief     Whether the raw ring holds keys the slave has not taken.
     *
     * @return    True when the slave's discipline has work waiting.
     * @since     0.5.0
     * @ingroup   kernel_driver
     */
    [[nodiscard]] bool raw_has_keys() const;

    /**
     * @brief     Pins one independent master open description.
     * @return    None
     * @since     0.6.0
     * @ingroup   kernel_driver
     */
    void retain_master();

    /**
     * @brief     Hangs up slave input when the last master open closes.
     * @return    None
     * @since     0.6.0
     * @ingroup   kernel_driver
     */
    void release_master();

    /**
     * @brief     Pins one independent slave open description.
     * @return    None
     * @since     0.6.0
     * @ingroup   kernel_driver
     */
    void retain_slave();

    /**
     * @brief     Makes master EOF visible at the last slave close.
     * @return    None
     * @since     0.6.0
     * @ingroup   kernel_driver
     */
    void release_slave();

    /**
     * @brief     Whether the master has closed its last open description.
     * @return    True when slave input is hung up.
     * @since     0.6.0
     * @ingroup   kernel_driver
     */
    [[nodiscard]] bool master_closed() const { return slave_.hung_up(); }

    /**
     * @brief     Whether a previously opened slave has no opens left.
     * @return    True between the last slave close and a subsequent open.
     * @since     0.6.0
     * @ingroup   kernel_driver
     */
    [[nodiscard]] bool slave_closed() const { return slave_opens_.load() == 0; }

    /**
     * @brief     Resets the pair to its born state, rings drained.
     *
     * @return    None
     * @note      Called when a freed slot returns to the table and a new
     *              pair takes the seat, so no old byte survives.
     * @since     0.5.0
     * @ingroup   kernel_driver
     */
    void reset();

private:
    /// One raw key from the master's writes, for the discipline's take.
    static bool take_raw(void* context, char* out);

    /// Whether the raw ring holds a key.
    static bool peek_raw(void* context);

    /// One echo character from the discipline, into the master ring.
    static void push_echo(void* context, char key);

    base::container::RingQueue<unsigned char, kPtyRawBytes + 1>    raw_;
    base::container::RingQueue<unsigned char, kPtyMasterBytes + 1> out_;
    TtyWiring                                                      wiring_;
    Tty                                                            slave_;
    std::atomic<unsigned long>                                     master_opens_{0};
    std::atomic<unsigned long>                                     slave_opens_{kSlaveNeverOpened};

    /// Before the first slave open, an empty master still waits for data.
    static constexpr unsigned long kSlaveNeverOpened = ~0UL;
};

}  // namespace cinux::driver
