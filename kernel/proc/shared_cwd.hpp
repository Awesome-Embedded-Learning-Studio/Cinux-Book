/**
 * @file kernel/proc/shared_cwd.hpp
 * @brief Reference-counted current working directory (F3-M2 batch 3)
 *
 * CLONE_FS threads share one SharedCwd instance (acquire bumps the refcount);
 * fork and clone-without-FS get a private copy.  Heap-allocated (slab general
 * cache) so a Task holds a pointer and clone sharing is pointer + acquire().
 *
 * Split out of process.hpp (F3-M2 batch 5) to keep process.hpp under the
 * 500-line soft limit.
 *
 * Namespace: cinux::proc
 */

#pragma once

#include <stdint.h>

namespace cinux::proc {

struct SharedCwd {
    static constexpr uint32_t kPathMax = 256;

    uint32_t refcount;
    char     path[kPathMax];

    /// Allocate with @p init (defaults to "/"); refcount = 1.
    static SharedCwd* create(const char* init = "/") {
        auto* p = new SharedCwd;
        if (p != nullptr) {
            p->refcount  = 1;
            uint32_t i   = 0;
            char*    dst = p->path;
            if (init != nullptr) {
                for (; i + 1 < kPathMax && init[i] != '\0'; ++i) {
                    dst[i] = init[i];
                }
            }
            dst[i] = '\0';
        }
        return p;
    }

    /// Allocate a private copy of @p src; refcount = 1.
    static SharedCwd* create_copy(const SharedCwd* src) {
        return create(src != nullptr ? src->path : "/");
    }

    // F4-M5 R3: atomic refcount.  CLONE_FS threads on different CPUs share one
    // SharedCwd (F3-M2), so acquire/release race once APs really run threads
    // (F4-M4).  A non-atomic ++/-- loses updates -> use-after-free or leak.
    // ACQ_REL: the release that brings refcount to 0 must see all prior writes
    // to the shared object before delete; acquire pairs with it.
    void acquire() { __atomic_add_fetch(&refcount, 1, __ATOMIC_ACQ_REL); }

    void release() {
        if (__atomic_sub_fetch(&refcount, 1, __ATOMIC_ACQ_REL) == 0) {
            delete this;
        }
    }
};

}  // namespace cinux::proc
