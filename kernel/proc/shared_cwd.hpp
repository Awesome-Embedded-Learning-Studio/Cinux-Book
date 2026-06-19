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

    void acquire() { ++refcount; }

    void release() {
        if (refcount > 0 && --refcount == 0) {
            delete this;
        }
    }
};

}  // namespace cinux::proc
