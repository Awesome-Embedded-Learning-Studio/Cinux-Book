/**
 * @file refcount.hpp
 * @brief RefCount — saturating atomic reference counter (Linux refcount_t semantics).
 *
 * Models heap-object lifetime, aligned with the Linux kernel's refcount_t
 * (include/linux/refcount.h): the counter saturates at kRefcountSaturated and
 * will not move once there. This avoids the wrapping that turns a plain atomic
 * refcount into a spurious use-after-free (a dec past 0 wraps to a large
 * positive value -> a later acquire thinks the object is still live; or wraps
 * back to 0 -> a double free).
 *
 * The saturation value is INT_MIN/2 (not INT_MIN): placed roughly equidistant
 * from 0 and INT_MAX, it minimises the chance that concurrent acquire/release
 * drift the counter into a dangerous region during the brief non-atomic window
 * between the fetch and the saturating clamp (mirrors Linux's design rationale).
 *
 * Uses GCC __atomic_* builtins (NOT std::atomic): a spike proved
 * std::atomic<uint32_t> drags in the libstdc++ symbol
 * std::__glibcxx_assert_fail, which fails to link under the kernel's
 * -ffreestanding -nostdlib. __atomic_* is lock-free inline on x86-64 (zero
 * external symbols) and works both in the kernel and in host tests.
 *
 * SCOPE / BOUNDARIES (read before reuse):
 * - Models **heap-object lifetime** (acquire/release the right to keep a shared
 *   object alive). release()==true means "last reference gone — you free it"
 *   (caller does `delete this` / cleanup). Intended consumers: Task,
 *   AddressSpace, SharedCwd, FDTable (F-QA Q4b-e).
 * - **Not** for per-page mapcount (DEBT-003): a physical page's "how many PTEs
 *   reference me" is a dense per-page int16 field on PMM metadata, not an
 *   object-lifetime counter.
 * - acquire() uses RELAXED and does **not** publish visibility of other object
 *   fields; callers rely on the publisher's release store (standard
 *   publish-via-release pattern).
 * - **Simple** lifetime counter (release-to-zero => caller frees). No on-zero
 *   resource-cleanup hook; objects whose release must run arbitrary cleanup
 *   (e.g. FDTable closing its file slots) do that at the call site after
 *   release()==true, not inside RefCount.
 *
 * No WARN mechanism (Cinux-Base is a pure zero-coupling library): saturation is
 * applied silently. Kernel consumers wanting observability may wrap
 * acquire/release and klog_warn on the saturated path.
 *
 * Namespace: cinux::lib
 */

#ifndef CINUX_REFCOUNT_HPP
#define CINUX_REFCOUNT_HPP

#include <cstdint>

namespace cinux::lib {

/// @brief Saturating threshold (Linux REFCOUNT_SATURATED = INT_MIN/2).
///
/// Placed roughly equidistant from 0 and INT_MAX so that concurrent drift
/// during the fetch-then-clamp window is least likely to reach a dangerous
/// value (0 = spurious free, or a wrapped large positive = spurious liveness).
constexpr uint32_t kRefcountSaturated = 0xC0000000u;  // INT_MIN / 2

/// @brief Saturating atomic reference counter. See @file header for scope,
///        boundaries, and the saturation rationale.
class RefCount {
public:
    /// Construct with an initial count of 1 (the creator's reference).
    constexpr RefCount() : count_(1) {}

    /// Construct with an explicit @p initial count.
    constexpr explicit RefCount(uint32_t initial) : count_(initial) {}

    RefCount(const RefCount&)            = delete;
    RefCount& operator=(const RefCount&) = delete;

    /// @brief Bump the reference count, saturating.
    ///
    /// RELAXED: acquire does not itself publish visibility of other object
    /// fields. If the counter is already saturated (or called on a 0 = UAF),
    /// it is clamped to kRefcountSaturated and the bump is discarded.
    void acquire() {
        uint32_t old = __atomic_fetch_add(&count_, 1, __ATOMIC_RELAXED);
        // fetch-then-clamp (mirrors Linux __refcount_add): the add already
        // happened, so a concurrent op may have moved the value transiently;
        // detect a bad pre-state and force-saturate rather than rolling back.
        //   old == 0                   -> add-on-freed (UAF)
        //   old >= kRefcountSaturated  -> already saturated / damaged
        if (old == 0 || old >= kRefcountSaturated) {
            __atomic_store_n(&count_, kRefcountSaturated, __ATOMIC_RELAXED);
        }
    }

    /// @brief Drop a reference.
    /// @return true if the count reached 0 — the caller now owns cleanup/free.
    ///
    /// On underflow (count was already 0 or saturated) the counter is clamped
    /// to kRefcountSaturated and false is returned, defending against UAF from
    /// a stray extra release. The release-to-zero path carries an acquire
    /// (compiler) fence so the freeing caller sees all prior writes by other
    /// references (x86 TSO needs no hardware fence; the barrier is for the
    /// compiler and weak-model portability).
    bool release() {
        uint32_t old = __atomic_fetch_sub(&count_, 1, __ATOMIC_RELEASE);
        if (old == 1) {
            // Reached zero: pair this acquire with the RELEASE stores of all
            // prior references so the freeing caller sees their writes.
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            return true;
        }
        //   old == 0                   -> underflow (UAF)
        //   old >= kRefcountSaturated  -> saturated
        if (old == 0 || old >= kRefcountSaturated) {
            __atomic_store_n(&count_, kRefcountSaturated, __ATOMIC_RELAXED);
        }
        return false;
    }

    /// @brief Read the current count (diagnostic only).
    uint32_t load() const { return __atomic_load_n(&count_, __ATOMIC_RELAXED); }

private:
    uint32_t count_;
};

}  // namespace cinux::lib

#endif  // CINUX_REFCOUNT_HPP
