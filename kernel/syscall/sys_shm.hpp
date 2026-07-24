/**
 * @file kernel/syscall/sys_shm.hpp
 * @brief SysV shared memory syscall handlers (F8-M4)
 *
 * Thin syscall boundaries over the ShmRegistry (kernel/ipc/shm.hpp) plus the
 * physical-page plumbing (PMM alloc/free + per-address-space mapping).  The
 * handlers own the page lifecycle the registry deliberately does not:
 *   - shmget allocates the frames and registers the segment;
 *   - shmat maps those frames into the current address space (eager, not
 *     demand-paged) and bumps each page's pte_count;
 *   - shmdt tears the mapping down (the segment retains the pages until the
 *     last attachment plus IPC_RMID);
 *   - shmctl implements IPC_STAT (kernel-to-kernel variant exposed for tests)
 *     and IPC_RMID.
 *
 * Namespace: cinux::syscall
 */

#pragma once

#include <stdint.h>

#include "kernel/arch/x86_64/syscall.hpp"
#include "kernel/ipc/shm.hpp"

namespace cinux::syscall {

/// shmget(key, size, shmflg) -- create or locate a shared memory segment.
/// Returns the shmid, or -errno.
int64_t sys_shmget(uint64_t key, uint64_t size, uint64_t shmflg, uint64_t, uint64_t, uint64_t);

/// shmat(shmid, addr, shmflg) -- map a segment into the current address space.
/// Returns the mapped virtual address, or -errno.
int64_t sys_shmat(uint64_t shmid, uint64_t addr, uint64_t shmflg, uint64_t, uint64_t, uint64_t);

/// shmdt(addr) -- unmap a previously attached segment.  Returns 0 or -errno.
int64_t sys_shmdt(uint64_t addr, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

/// shmctl(shmid, cmd, buf) -- IPC_STAT / IPC_RMID.  Returns 0 or -errno.
int64_t sys_shmctl(uint64_t shmid, uint64_t cmd, uint64_t buf, uint64_t, uint64_t, uint64_t);

/**
 * @brief Kernel-to-kernel shmctl (no user memory)
 *
 * IPC_STAT fills @p out directly (the layer tests target); IPC_RMID marks the
 * segment and frees its pages when no attachment remains.  Mirrors the
 * do_stat_kernel / sys_stat split so the ring-0 test exercises IPC_STAT without
 * a user buffer.
 *
 * @return 0 on success, or -errno.
 */
int64_t do_shmctl_kernel(uint64_t shmid, uint64_t cmd, cinux::ipc::shmid_ds* out);

}  // namespace cinux::syscall
