/**
 * @file kernel/drivers/tty/pty_device.hpp
 * @brief PTY device layer: registry of pairs + master/slave/ptmx InodeOps
 *        (F10-M3 Phase 2 batch 3)
 *
 * Bridges the pure-logic Pty (batch 1) to the VFS device-inode model so a
 * process can open /dev/ptmx, drive the master fd, and open /dev/pts/<n> for
 * the slave fd -- the Linux PTY ABI.
 *
 *   - pty_alloc()         : allocate a fresh pair, return its index.
 *   - ptmx_ops()          : the /dev/ptmx node; its open() clones (allocates a
 *                           pair and returns the master inode for the new fd).
 *   - pty_master_inode()  : the master inode of pair @p index (fd side: the
 *                           terminal emulator reads/writes here).
 *   - pty_slave_inode()   : /dev/pts/<index> lookup -> the slave inode (the
 *                           application's controlling terminal side).
 *
 * A fixed table (kMaxPtys) keeps this simple and deterministic; pairs are
 * reused on close-and-realloc.  Kernel-only: the slave ioctl crosses the user
 * boundary (copy_to/from_user), so this unit is not host-testable -- the pure
 * pair logic is covered by the batch-1 host tests, and this wiring by the
 * batch-3 kernel test.
 *
 * Namespace: cinux::drivers
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <cinux/expected.hpp>

#include "kernel/fs/inode.hpp"

namespace cinux::fs {
class InodeOps;
}

namespace cinux::drivers {

/// Maximum simultaneously allocated PTY pairs.  Hobby-OS bound; Linux allocates
/// dynamically, but a fixed table is simple and deterministic.
static constexpr size_t kMaxPtys = 8;

/// Linux TIOCGPTN: read the pty number of a master fd (so libc can open the
/// matching /dev/pts/<N>).  Shared by the master ioctl and the test.
constexpr uint32_t kTiocgptn = 0x80045430;

/// Allocate a new PTY pair and return its index (0..kMaxPtys-1), or an error
/// (OutOfMemory -> EMFILE) when the table is full.  Used by /dev/ptmx open().
cinux::lib::ErrorOr<int> pty_alloc();

/// Mark the pair at @p index free for reuse (test cleanup now; the future
/// close()-on-slave path will call this).  The Pty itself is re-initialised by
/// the next pty_alloc() on the slot.
void pty_release(int index);

/// The master inode of pair @p index (nullptr if not allocated).
cinux::fs::Inode* pty_master_inode(int index);

/// Resolve /dev/pts/<index> to the slave inode, or NotFound if no such pair.
cinux::lib::ErrorOr<cinux::fs::Inode*> pty_slave_inode(int index);

/// The /dev/ptmx device ops -- a cloning device whose open() allocates a pair
/// and returns the master inode.  DevFs registers "ptmx" against this.
cinux::fs::InodeOps& ptmx_ops();

}  // namespace cinux::drivers
