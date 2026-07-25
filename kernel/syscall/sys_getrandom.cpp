/**
 * @file kernel/syscall/sys_getrandom.cpp
 * @brief sys_getrandom handler (gcc/g++ self-host batch, 2026-07-05)
 *
 * Fills the user buffer with bytes from the kernel PRNG (KRandom, F9 batch 7).
 * glibc uses this for stack canaries, ASLR helpers, and fd_set perturbation.
 * flags (GRND_NONBLOCK / GRND_RANDOM) are accepted but ignored -- the kernel
 * PRNG never blocks and is the single random source.
 */

#include "kernel/syscall/sys_getrandom.hpp"

#include <stddef.h>
#include <stdint.h>

#include "kernel/arch/x86_64/user_access.hpp"  // copy_to_user
#include "kernel/errno.hpp"
#include "kernel/lib/random.hpp"  // g_random

namespace cinux::syscall {

int64_t sys_getrandom(uint64_t buf_virt, uint64_t len, uint64_t /*flags*/, uint64_t, uint64_t,
                      uint64_t) {
    if (buf_virt == 0 && len != 0) {
        return -cinux::kEfault;
    }
    // Stage random bytes through a kernel scratch buffer and copy out in chunks
    // (g_random.fill writes kernel memory; SMAP requires copy_to_user to push
    // it across the user boundary).
    constexpr size_t kChunk = 256;
    uint8_t          kbuf[kChunk];
    uint64_t         done = 0;
    while (done < len) {
        size_t n = (len - done < kChunk) ? static_cast<size_t>(len - done) : kChunk;
        cinux::lib::g_random.fill(kbuf, n);
        if (!cinux::user::copy_to_user(reinterpret_cast<void*>(buf_virt + done), kbuf, n)) {
            return -cinux::kEfault;
        }
        done += n;
    }
    return static_cast<int64_t>(len);
}

}  // namespace cinux::syscall
