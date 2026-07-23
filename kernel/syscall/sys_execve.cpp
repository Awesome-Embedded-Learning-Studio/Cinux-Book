/**
 * @file kernel/syscall/sys_execve.cpp
 * @brief sys_execve handler implementation (P0c SMAP-layered)
 *
 * Layered (Linux-aligned):
 *   - do_execve_kernel(kpath, kargv, kenvp): pure kernel-to-kernel ELF load +
 *     initial-stack lay + jump to user mode. Receives KERNEL strings (already
 *     staged), so it can unmap old user pages / block on VFS without holding
 *     any user pointer. Kernel-internal callers and tests use this.
 *   - sys_execve: the user boundary. It stages path/argv/envp out of user
 *     memory via accessors BEFORE do_execve_kernel, then calls it. copy_strvec
 *     uses TWO accessor layers: get_user reads each user pointer slot, then
 *     get_user reads each user string byte-by-byte to NUL. No raw dereference
 *     of user memory anywhere (the old "AC set by entry stub" assumption is
 *     gone -- global STAC will be removed in P3).
 *
 * execve never returns on success (the image is replaced); on failure it
 * returns -errno so the shell can report "not found".
 */

#include "kernel/syscall/sys_execve.hpp"

#include <stddef.h>
#include <stdint.h>

#include <memory>  // std::unique_ptr (freestanding kernel can use <memory>)

#include "kernel/arch/x86_64/user_access.hpp"  // P0c (SMAP): get_user / access_ok
#include "kernel/errno.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/proc/execve.hpp"       // execve, ExecveResult, ElfAuxInfo
#include "kernel/proc/user_launch.hpp"  // enter_loaded_program

namespace cinux::syscall {

namespace {

/// Max argument/env entries we will copy per vector (also bounds the on-stack
/// pointer arrays, keeping the handler frame small).
constexpr int    kMaxArgs     = 32;
/// Heap string pool for path + argv + envp strings (avoids kernel-stack pressure).
constexpr size_t kStrPoolSize = 16384;

/// Read one NUL-terminated user string into the pool via get_user (byte by
/// byte). Sets *out to the kernel copy. Returns false on access failure or if
/// the string does not fit (no NUL within pool_cap).
bool read_user_string(uint64_t virt, char* pool, size_t pool_cap, size_t& used, const char** out) {
    if (!cinux::user::access_ok(reinterpret_cast<const void*>(virt), 1)) {
        return false;
    }
    size_t len = 0;
    while (used + len + 1 < pool_cap) {
        char c;
        if (!cinux::user::get_user(&c, reinterpret_cast<const char*>(virt + len))) {
            return false;
        }
        if (c == '\0') {
            break;
        }
        pool[used + len] = c;
        ++len;
    }
    char term = 0;
    if (!cinux::user::get_user(&term, reinterpret_cast<const char*>(virt + len))) {
        return false;
    }
    if (term != '\0') {
        return false;  // no NUL within pool_cap (string too long / pool full)
    }
    pool[used + len] = '\0';
    *out             = pool + used;
    used += len + 1;
    return true;
}

/// Copy up to kMaxArgs user strings (a user pointer array + user strings) into
/// the pool via TWO accessor layers: get_user reads each pointer slot, get_user
/// reads each string byte. Returns the entry count, or -1 on access failure /
/// overflow. Never raw-dereferences user memory.
int copy_strvec(uint64_t user_virt, const char** out, char* pool, size_t pool_cap, size_t& used) {
    int n = 0;
    if (user_virt != 0) {
        if (!cinux::user::access_ok(reinterpret_cast<const void*>(user_virt), 8)) {
            return -1;
        }
        for (; n < kMaxArgs; ++n) {
            // Layer 1: read the user pointer slot (one const char* per entry).
            uint64_t slot = 0;
            if (!cinux::user::get_user(&slot,
                                       reinterpret_cast<const uint64_t*>(user_virt + n * 8))) {
                return -1;
            }
            if (slot == 0) {
                break;  // nullptr terminator
            }
            // Layer 2: read the user string that slot points at.
            const char* s = nullptr;
            if (!read_user_string(slot, pool, pool_cap, used, &s)) {
                return -1;
            }
            out[n] = s;
        }
    }
    out[n] = nullptr;
    return n;
}

}  // anonymous namespace

// ============================================================
// do_execve_kernel: pure kernel-to-kernel (kernel path/argv/envp -> ELF load)
// ============================================================

int64_t do_execve_kernel(const char* kpath, const char* const* kargv, const char* const* kenvp) {
    cinux::lib::kprintf("[EXECVE] loading '%s'\n", kpath);
    cinux::proc::ElfAuxInfo elf_aux{};
    auto                    result = cinux::proc::execve(kpath, kargv, kenvp, &elf_aux);
    cinux::lib::kprintf("[EXECVE] execve result=%d entry=%p\n", static_cast<int>(result),
                        reinterpret_cast<void*>(elf_aux.at_entry));
    if (result != cinux::proc::ExecveResult::Ok) {
        return static_cast<int64_t>(result);
    }
    // Success: replace this image and resume at the new entry. Never returns;
    // any pool leak is intentional (the old image's stack is gone).
    cinux::lib::kprintf("[EXECVE] entering loaded program\n");
    cinux::proc::enter_loaded_program(kpath, kargv, kenvp, elf_aux);
    return 0;  // unreachable
}

// ============================================================
// sys_execve boundary: stage user path/argv/envp via accessors -> do_execve_kernel
// ============================================================

int64_t sys_execve(uint64_t path_virt, uint64_t argv_virt, uint64_t envp_virt, uint64_t, uint64_t,
                   uint64_t) {
    cinux::lib::kprintf(
        "[EXECVE] sys_execve path=%p argv=%p envp=%p\n", reinterpret_cast<const void*>(path_virt),
        reinterpret_cast<const void*>(argv_virt), reinterpret_cast<const void*>(envp_virt));

    // Stage path/argv/envp into kernel memory via accessors BEFORE execve()
    // unmaps the user pages they live in. Pool is heap-allocated.
    auto pool = std::unique_ptr<char[]>(new char[kStrPoolSize]);
    if (pool == nullptr) {
        return -cinux::kEnomem;
    }
    char*  pool_base = pool.get();
    size_t used      = 0;

    const char* kpath               = nullptr;
    const char* kargv[kMaxArgs + 1] = {};
    const char* kenvp[kMaxArgs + 1] = {};

    if (!read_user_string(path_virt, pool_base, kStrPoolSize, used, &kpath) || kpath == nullptr) {
        cinux::lib::kprintf("[EXECVE] copy path failed -> EFAULT\n");
        return -cinux::kEfault;
    }
    int argc = copy_strvec(argv_virt, kargv, pool_base, kStrPoolSize, used);
    int envc = copy_strvec(envp_virt, kenvp, pool_base, kStrPoolSize, used);
    cinux::lib::kprintf("[EXECVE] copy argc=%d envc=%d\n", argc, envc);
    if (argc < 0 || envc < 0) {
        cinux::lib::kprintf("[EXECVE] copy failed -> EINVAL\n");
        return -cinux::kEinval;  // argv/envp too large / inaccessible
    }

    return do_execve_kernel(kpath, kargv, kenvp);
}

}  // namespace cinux::syscall
