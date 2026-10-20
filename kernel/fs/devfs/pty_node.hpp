/**
 * @file    pty_node.hpp
 * @brief   Wiring the PTY pairs into devfs: ptmx the cloning seat, pts/N
 *          the slaves.
 *
 * The pair logic lives in the driver's Pty class; this is the devfs
 * side. One registration seats /dev/ptmx (5:2, Linux's pairing) and
 * installs the two resolvers: opening ptmx clones a fresh pair and hands
 * back its master, and looking up pts/N finds pair number N's slave.
 * Each side carries its own device faces — the master reads what the
 * slave produced, the slave is the terminal, termios questions on it
 * reach the real discipline.
 *
 * @author  Charliechen114514
 * @date    2026-10-19
 * @version 0.1
 * @since   0.5.0
 * @ingroup kernel_fs
 * @copyright Copyright (c) 2026
 */

#pragma once

namespace cinux::fs {

class DevFs;

/**
 * @brief         Seats ptmx and the pair resolvers in a devfs instance.
 *
 * @param[in,out] devfs   The table receiving the PTY seats.
 * @return        None
 * @note          Bring-up wiring, single-threaded, before the mount goes
 *                live. The pair table lives here, eight seats; a pair is
 *                minted on the first ptmx open that needs it and reset,
 *                never destroyed, when its seat returns.
 * @since         0.5.0
 * @ingroup       kernel_fs
 */
void RegisterPtyDevices(DevFs& devfs);

}  // namespace cinux::fs
