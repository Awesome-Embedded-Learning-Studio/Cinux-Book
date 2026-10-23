/**
 * @file    vma.hpp
 * @brief   The per-task register of user address ranges.
 *
 * A VMA (virtual memory area) is one recorded promise about a stretch of
 * the user half: where it starts and ends, what may be done there, and
 * what stands behind it. The register is the book the page-fault path
 * consults before judging any fault — a fault inside a registered range
 * is a page the process was promised, one outside is a true segmentation
 * fault. Registration arrives with exec (the segments and the stack) and
 * grows when mmap asks for anonymous memory; the fault-path promotion
 * that finally keeps those promises is its own station. The store is a
 * plain intrusive chain walked read-only: ranges number in the dozens,
 * so a linear scan is the honest cost, and no allocation hides in find.
 *
 * @author  Charliechen114514
 * @date    2026-10-14
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_mm
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "cinux/container/self_list.hpp"
#include "cinux/ref_count.hpp"
#include "cinux/result.hpp"
#include "kernel/fs/vfs.hpp"
#include "kernel/mm/layout.hpp"

namespace cinux::mm {

/**
 * @brief   What stands behind one registered range.
 * @since   0.1.0
 * @ingroup kernel_mm
 */
enum class VmaKind : unsigned char {
    kAnonymous,  ///< Fresh memory with no file behind it; faults refill from zero.
    kFile,       ///< Bytes that trace back to a file node, as exec's segments do.
    kStack,      ///< The main stack, which grows downward toward lower addresses.
};

/**
 * @brief   One recorded promise: a half-open range plus its terms.
 * @note    [start, end) on purpose — an empty range is expressible and
 *          refused at registration, and end belongs to the next range,
 *          never to this one. The backing node travels by value: it is
 *          the identity of the source, read by the fault path and by
 *          dumps, never dereferenced on the register's own authority.
 *          The face beside it — erased ops and the backend instance —
 *          is what turns that identity into bytes when a fault asks
 *          for a file page; without it a kFile range is a promise the
 *          fault path cannot keep, so exec and mmap fill both or
 *          neither.
 * @since   0.1.0
 * @ingroup kernel_mm
 */
struct VmaRegion : base::container::SelfNode<VmaRegion> {
    unsigned long      start{};        ///< First byte the promise covers.
    unsigned long      end{};          ///< One past the last covered byte.
    VmaKind            kind{};         ///< What stands behind the bytes.
    bool               readable{};     ///< Loads through this range are promised.
    bool               writable{};     ///< Stores through this range are promised.
    bool               executable{};   ///< Running code from this range is promised.
    fs::FsNode         backing{};      ///< The source node when kind is kFile.
    const fs::FsOps*   file_ops{};     ///< The backend face that serves backing's bytes.
    void*              file_self{};    ///< The backend instance behind file_ops.
    unsigned long long file_offset{};  ///< File byte matching start, when backed.
};

/**
 * @brief   The terms a promise may be rewritten under.
 * @since   0.2.0
 * @ingroup kernel_mm
 */
struct VmaProtection {
    bool readable{};    ///< Loads through the stretch are promised.
    bool writable{};    ///< Stores through the stretch are promised.
    bool executable{};  ///< Running code from the stretch is promised.
};

/**
 * @brief   The register itself: an intrusive chain of promises, one per task.
 * @note    Owned by the task through a pointer, because exec builds the
 *          next program's register beside the live one and swaps only on
 *          success. Cloning is the fork story: the child's register is a
 *          deep copy taken while every page is still shared. Ranges keep
 *          arrival order; the chain is short and find is linear by
 *          design, so there is no ordering invariant to maintain and no
 *          rebalancing story to teach.
 * @since   0.1.0
 * @ingroup kernel_mm
 */
class VmaStore : public cinux::base::RefCount<> {
public:
    VmaStore() = default;

    /**
     * @brief         Names this world's mmap ceiling.
     *
     * @param[in]     ceiling   Where anonymous mmap hunts begin; the
     *                          boot default is the compiled-in top.
     * @return        None.
     * @note          exec lowers the ceiling by one random page-multiple
     *                per world, so no two programs agree on where their
     *                          mappings start.
     * @since         0.3.0
     * @ingroup       kernel_mm
     */
    void set_mmap_ceiling(unsigned long ceiling) { mmap_ceiling_ = ceiling; }

    VmaStore(const VmaStore&)            = delete;
    VmaStore& operator=(const VmaStore&) = delete;

    /**
     * @brief         Records one promise about the user half.
     *
     * @param[in]     spec   The range and its terms.
     * @return        Ok, or kInvalidArgument when the range is empty or
     *                inverted, kAlreadyExists when it overlaps a recorded one.
     * @note          Overlap is refused, not merged: exec's segments never
     *                touch, and mmap hands out disjoint spots by
     *                construction, so an overlap names a caller bug and is
     *                reported rather than absorbed.
     * @warning       None
     * @throws        None
     * @since         0.1.0
     * @ingroup       kernel_mm
     */
    [[nodiscard]] base::Result<void> register_range(const VmaRegion& spec);

    /**
     * @brief         Finds the promise covering one address.
     *
     * @param[in]     address   The byte to look up.
     * @return        The covering region, nullptr when none does.
     * @warning       None
     * @throws        None
     * @since         0.1.0
     * @ingroup       kernel_mm
     */
    [[nodiscard]] const VmaRegion* find(unsigned long address) const;

    [[nodiscard]] bool stack_contains(unsigned long start, unsigned long bytes) const;

    /**
     * @brief         Builds an independent copy of the whole register.
     *
     * @return        The copy, nullptr when an allocation refused.
     * @note          All-or-nothing: a copy that stops halfway hands back
     *                nullptr with every partial node returned, so fork can
     *                treat one null as its whole rollback signal.
     * @warning       None
     * @throws        None
     * @since         0.1.0
     * @ingroup       kernel_mm
     */
    [[nodiscard]] VmaStore* clone() const;

    /**
     * @brief         Returns every recorded region to the heap.
     *
     * @return        None.
     * @note          Also the destructor's body: a register that goes out
     *                of scope cleans up after itself, so owners delete and
     *                nothing more.
     * @warning       None
     * @throws        None
     * @since         0.1.0
     * @ingroup       kernel_mm
     */
    void teardown();

    /**
     * @brief         The destructor's share of the lifecycle.
     * @return        None
     * @note          None
     * @warning       None
     * @throws        None
     * @since         0.1.0
     * @ingroup       kernel_mm
     */
    ~VmaStore() { teardown(); }

    /**
     * @brief         Whether any promise is recorded.
     *
     * @return        True when the register holds no region.
     * @warning       None
     * @throws        None
     * @since         0.1.0
     * @ingroup       kernel_mm
     */
    [[nodiscard]] bool empty() const { return regions_.empty(); }

    /**
     * @brief         How many promises the register holds.
     *
     * @return        Chain region count; the brk span is not counted.
     * @note          The overview face for the panic dump: a number a
     *                human compares against what the program meant to
     *                map, without walking the chain itself.
     * @warning       None
     * @throws        None
     * @since         0.3.0
     * @ingroup       kernel_mm
     */
    [[nodiscard]] unsigned long long region_count() const { return regions_.size(); }

    /**
     * @brief         Names the heap span this world grows by brk.
     *
     * @param[in]     base   Where the program image ends; the break starts
     *                       there and only moves up.
     * @param[in]     max    The highest address the break may reach.
     * @return        None.
     * @note          The heap is not a chain region: its length starts at
     *                zero, and an empty range cannot register. It rides as
     *                its own member the fault path reads through find(),
     *                so a brk page and an mmap page ask the register the
     *                same question and get the same shape of answer.
     * @warning       None
     * @throws        None
     * @since         0.2.0
     * @ingroup       kernel_mm
     */
    void seed_heap(unsigned long base, unsigned long max);

    /**
     * @brief         Moves the break, Linux fashion.
     *
     * @param[in]     target   The break wanted; 0 only asks for the
     *                         current one.
     * @return        The break after the move, or kNotFound when the
     *                world never seeded a heap.
     * @note          No page is mapped or unmapped here — the cursor and
     *                the span's end move together, and pages inside the
     *                span arrive when their faults are fulfilled. The
     *                target is page-aligned up and clamped into
     *                [base, max] before anything moves.
     * @warning       None
     * @throws        None
     * @since         0.2.0
     * @ingroup       kernel_mm
     */
    [[nodiscard]] base::Result<unsigned long> move_heap_end(unsigned long target);

    /**
     * @brief         The break as it stands.
     *
     * @return        The current break, or 0 before seed_heap.
     * @warning       None
     * @throws        None
     * @since         0.2.0
     * @ingroup       kernel_mm
     */
    [[nodiscard]] unsigned long heap_end() const { return heap_.end; }

    /// Where this world's anonymous mmap hunts begin; the boot default
    /// is the compiled-in top until exec lowers it.
    [[nodiscard]] unsigned long mmap_ceiling() const { return mmap_ceiling_; }

    /**
     * @brief         Finds a free stretch below a ceiling, growing down.
     *
     * @param[in]     ceiling   The address the stretch must end at or
     *                          below; page-aligned.
     * @param[in]     bytes     How many bytes are wanted; page-aligned.
     * @return        The stretch's first byte, or kOutOfMemory when no
     *                gap that wide sits below the ceiling.
     * @note          The mmap-area allocator: the hunt starts at the
     *                ceiling and every recorded span in the way — chain
     *                region or heap — pushes the candidate below itself.
     *                A full pass that moves nothing means the spot is
     *                genuinely clear.
     * @warning       None
     * @throws        None
     * @since         0.2.0
     * @ingroup       kernel_mm
     */
    [[nodiscard]] base::Result<unsigned long> find_free_spot(unsigned long ceiling,
                                                             unsigned long bytes) const;

    /**
     * @brief         Withdraws every promise fully inside a stretch.
     *
     * @param[in]     start   First byte of the stretch.
     * @param[in]     end     One past the last byte.
     * @return        Ok, kInvalidArgument when a recorded span overlaps
     *                the stretch only partly — Linux answers munmap of a
     *                partly-mapped stretch with ENOMEM, and so does the
     *                register.
     * @note          Chain regions leave the chain; a fully withdrawn
     *                heap empties its span and the break collapses to its
     *                base. Which translations still name pages of the
     *                withdrawn ranges is the caller's separate question.
     * @warning       None
     * @throws        None
     * @since         0.2.0
     * @ingroup       kernel_mm
     */
    [[nodiscard]] base::Result<void> unregister_range(unsigned long start, unsigned long end);

    /**
     * @brief         Rewrites the terms of every promise inside a stretch.
     *
     * @param[in]     start   First byte of the stretch.
     * @param[in]     end     One past the last byte.
     * @param[in]     terms   The new terms.
     * @return        Ok, kInvalidArgument when a page of the stretch
     *                belongs to no recorded span.
     * @note          The stretch may cut through a promise: a span's
     *                words move only when the stretch covers it whole
     *                or sits inside it, and a straddling cut rewrites
     *                tables alone (the caller's page walk), leaving
     *                the span's own words as they were. The register is
     *                the promise; this changes the promise.
     * @warning       None
     * @throws        None
     * @since         0.2.0
     * @ingroup       kernel_mm
     */
    [[nodiscard]] base::Result<void> protect(unsigned long start, unsigned long end,
                                             VmaProtection terms);

    /**
     * @brief         One past the highest recorded byte.
     *
     * @return        The highest end among chain regions and the heap.
     * @note          Where exec plants the break: the image ends, the
     *                break starts.
     * @warning       None
     * @throws        None
     * @since         0.2.0
     * @ingroup       kernel_mm
     */
    [[nodiscard]] unsigned long max_end() const;

private:
    /**
     * @brief         Whether a stretch touches only spans it may wholly own.
     * @param[in]     start   First byte of the stretch.
     * @param[in]     end     One past the last byte.
     * @return        True when no chain region overlaps the stretch
     *                without sitting fully inside it, and the heap
     *                answers the same way.
     * @note          The gate protect and unregister share: both may
     *                rewrite or withdraw whole spans, and neither may
     *                split one.
     * @since         0.2.0
     * @ingroup       kernel_mm
     */
    [[nodiscard]] bool movable_within(unsigned long start, unsigned long end) const;

    /**
     * @brief         Whether every page of a stretch belongs to a span
     *                the register knows.
     * @param[in]     start   First byte of the stretch.
     * @param[in]     end     One past the last byte.
     * @return        True when the heap or the chain covers the stretch
     *                with no unmapped hole.
     * @note          The gate protect answers to instead of the whole-span
     *                rule: rewriting terms never withdraws a promise, so
     *                a stretch that cuts through spans is still sound.
     * @since         0.4.0
     * @ingroup       kernel_mm
     */
    [[nodiscard]] bool span_known(unsigned long start, unsigned long end) const;

    base::container::SelfList<VmaRegion> regions_;         ///< The promises, arrival order.
    VmaRegion                            heap_{};          ///< The brk span, empty until seeded.
    unsigned long                        heap_max_{};      ///< The break's ceiling from exec.
    unsigned long mmap_ceiling_{cinux::mm::kUserMmapTop};  ///< Where anonymous mmap hunts begin.
};

}  // namespace cinux::mm
