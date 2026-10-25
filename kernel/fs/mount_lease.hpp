/**
 * @file mount_lease.hpp
 * @brief Runtime backend ownership across unmount.
 * @author Charliechen114514
 * @date 2026-10-25
 * @version 0.1
 * @since 0.8.0
 * @ingroup kernel_fs
 * @copyright Copyright (c) 2026
 */
#pragma once
#include "cinux/ref_count.hpp"

namespace cinux::fs {
/** @brief Owns one backend until all resolved views and opens retire. */
struct MountLease : base::RefCount<> {
    void* backend{};           ///< Owned backend instance.
    void (*destroy)(void*){};  ///< Concrete backend destructor.
    /** @brief Releases the owned backend. */
    ~MountLease() {
        if (destroy != nullptr) {
            destroy(backend);
        }
    }
};
}  // namespace cinux::fs
