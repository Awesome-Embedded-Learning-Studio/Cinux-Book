/**
 * @file kernel/syscall/sys_execve.cpp
 * @brief sys_execve handler implementation
 *
 * execve() semantically never returns: it replaces the current image and
 * resumes at the new program's entry.  Because argv/envp/path arrive as USER
 * pointers into the caller's soon-to-be-discarded image, the handler copies
 * them into kernel buffers FIRST, then loads the ELF via cinux::proc::execve()
 * (which unmaps the old user pages and fills the auxv info), then calls
 * enter_loaded_program() to lay the Linux initial stack (argc/argv/envp/auxv)
 * and jump to user mode.  On failure it returns a negative code so the caller
 * (shell) can report "not found".
 */

#include "kernel/syscall/sys_execve.hpp"

#include <memory>   // std::unique_ptr (freestanding kernel can use <memory>)
#include <stddef.h>

#include "kernel/errno.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/proc/execve.hpp"      // execve, ExecveResult, ElfAuxInfo
#include "kernel/proc/user_launch.hpp" // enter_loaded_program

namespace cinux::syscall {

namespace {

/// Max argument/env entries we will copy per vector (also bounds the on-stack
/// pointer arrays, keeping the handler frame small).
constexpr int kMaxArgs = 32;
/// Heap string pool for path + argv + envp strings (avoids kernel-stack pressure).
constexpr size_t kStrPoolSize = 16384;

/// Copy up to kMaxArgs NUL-terminated strings from user array @p user into @p
/// pool, writing each string's kernel pointer into @p out (NUL-terminated).
/// @p used accumulates pool bytes consumed. Returns the entry count, or -1 on
/// overflow. Reads user memory (SMAP AC is set by the syscall entry stub).
int copy_strvec(const char* const* user, const char** out, char* pool, size_t pool_cap,
                size_t& used) {
    int n = 0;
    if (user != nullptr) {
        for (; n < kMaxArgs; ++n) {
            const char* s = user[n];
            if (s == nullptr) {
                break;
            }
            size_t len = 0;
            while (len < pool_cap - used && s[len] != '\0') {
                ++len;
            }
            if (s[len] != '\0') {
                return -1;  // string too long / pool full
            }
            char* dst = pool + used;
            for (size_t k = 0; k <= len; ++k) {
                dst[k] = s[k];
            }
            out[n] = dst;
            used += len + 1;
        }
    }
    out[n] = nullptr;
    return n;
}

}  // anonymous namespace

int64_t sys_execve(uint64_t path_virt, uint64_t argv_virt, uint64_t envp_virt, uint64_t, uint64_t,
                   uint64_t) {
    const char*        upath = reinterpret_cast<const char*>(path_virt);
    const char* const* uargv = reinterpret_cast<const char* const*>(argv_virt);
    const char* const* uenvp = reinterpret_cast<const char* const*>(envp_virt);

    cinux::lib::kprintf("[EXECVE] sys_execve path=%p argv=%p envp=%p\n",
                        reinterpret_cast<const void*>(upath), reinterpret_cast<const void*>(uargv),
                        reinterpret_cast<const void*>(uenvp));

    // Copy path/argv/envp into kernel memory before execve() unmaps the user
    // pages they live in.  The string pool is heap-allocated (two 64-pointer
    // arrays on the stack are fine; the strings are not).
    auto pool = std::unique_ptr<char[]>(new char[kStrPoolSize]);
    if (pool == nullptr) {
        return -cinux::kEnomem;
    }
    char*  pool_base = pool.get();
    size_t used = 0;
    const char* path_vec[2] = {upath, nullptr};
    const char* kpath[2]    = {nullptr, nullptr};
    const char* kargv[kMaxArgs + 1];
    const char* kenvp[kMaxArgs + 1];

    int path_n = copy_strvec(path_vec, kpath, pool_base, kStrPoolSize, used);
    int argc   = copy_strvec(uargv, kargv, pool_base, kStrPoolSize, used);
    int envc   = copy_strvec(uenvp, kenvp, pool_base, kStrPoolSize, used);
    cinux::lib::kprintf("[EXECVE] copy path_n=%d argc=%d envc=%d\n", path_n, argc, envc);
    if (path_n < 0 || argc < 0 || envc < 0 || kpath[0] == nullptr) {
        cinux::lib::kprintf("[EXECVE] copy failed -> EINVAL\n");
        return -cinux::kEinval;  // path/argv/envp too large
    }

    cinux::lib::kprintf("[EXECVE] loading '%s'\n", kpath[0]);
    cinux::proc::ElfAuxInfo elf_aux{};
    auto result = cinux::proc::execve(kpath[0], kargv, kenvp, &elf_aux);
    cinux::lib::kprintf("[EXECVE] execve result=%d entry=%p\n", static_cast<int>(result),
                        reinterpret_cast<void*>(elf_aux.at_entry));
    if (result != cinux::proc::ExecveResult::Ok) {
        return static_cast<int64_t>(result);
    }

    // Success: replace this image and resume at the new entry. Never returns;
    // the pool leak here is intentional (the old image's stack is gone).
    cinux::lib::kprintf("[EXECVE] entering loaded program\n");
    cinux::proc::enter_loaded_program(kpath[0], kargv, kenvp, elf_aux);
    return 0;  // unreachable
}

}  // namespace cinux::syscall
