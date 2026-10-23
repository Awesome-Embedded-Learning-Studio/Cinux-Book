/**
 * @file    futex.hpp
 * @brief   The wait queue keyed by a user word, as ring 3 sees it.
 *
 * Futex is the handshake pthread_join and the mutex family stand on:
 * a thread names a user-space word and an expected value, the kernel
 * parks it only while the word still holds that value, and a wake
 * side releases waiters by naming the same word. The death side of
 * that handshake is here too: a dying thread leaves a zero in the
 * word its joiner watches and releases it. This header is the
 * announce face only — three calls; the bucket table and the
 * matching key live in the implementation file.
 *
 * @author  Charliechen114514
 * @date    2026-10-17
 * @version 0.2
 * @since   0.8.0
 * @ingroup kernel_proc
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "cinux/result.hpp"

namespace cinux::proc {
struct Task;
}  // namespace cinux::proc

namespace cinux::proc::futex {

/**
 * @brief         Park the caller while a user word holds a value.
 * @param[in]     uaddr      The user word's address, four-byte aligned.
 * @param[in]     expected   The value to hold the caller for.
 * @return        kOk once another side woke the caller, kWouldBlock
 *                when the word moved on before the park.
 * @note          The word is read under the table lock; a faulting
 *                address reaches the page-fault path from there, the
 *                bounds check stays the caller's (the syscall
 *                corridor's, in practice).
 * @since         0.8.0
 * @ingroup       kernel_proc
 */
base::KernelError ParkOnWord(unsigned long long uaddr, unsigned int present, unsigned int expected);
/**
 * @brief         Parks the caller on a user word after reading it through
 *                the accessor door.
 *
 * @param[in]     uaddr     The word's user address.
 * @param[in]     expected  The value the caller believes the word holds.
 * @return        Ok when woken; WouldBlock when the word already moved;
 *                InvalidArgument when the door refuses the address.
 * @note          The read happens before the engine takes its lock: the
 *                SMAP window may never span a spin, so the word crosses
 *                the door first and the engine parks on the address and
 *                the value it brought.
 * @since         0.3.0
 * @ingroup       kernel_proc
 */
base::KernelError WaitOnUserWord(unsigned long long uaddr, unsigned int expected);

/**
 * @brief         Wake up to a count of waiters parked on one word.
 * @param[in]     uaddr   The word whose waiters should resume.
 * @param[in]     count   At most this many waiters wake.
 * @return        How many were woken; zero when nobody waits there.
 * @note          Matching is by exact address, not by bucket — two
 *                words sharing a bucket never wake each other.
 * @since         0.8.0
 * @ingroup       kernel_proc
 */
long long WakeUserWord(unsigned long long uaddr, unsigned long long count);

/**
 * @brief         Release a dying task's join word: zero it, wake one.
 * @param[in]     dying   The task on its way out of the machine.
 * @return        None.
 * @note          Runs while the dying task's own address space is
 *                still the active one, so the store lands in the world
 *                its joiner shares. A word the task never named, one
 *                outside the user half, or one no writable promise
 *                covers is left untouched — a dying task must not
 *                fault. The word is per address space, so a process
 *                that clears its own private copy simply goes unseen.
 * @since         0.9.0
 * @ingroup       kernel_proc
 */
void ClearTidWord(Task& dying);

}  // namespace cinux::proc::futex
