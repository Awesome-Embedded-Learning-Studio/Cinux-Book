/**
 * @file    demand.hpp
 * @brief   The not-present half of the page-fault resolver.
 *
 * The register (vma.hpp) records what was promised; this is the half
 * that keeps those promises one page at a time. CoW (cow.hpp) owns the
 * write-against-shared-page slice of faults; everything else that can
 * be repaired is a page that was never mapped, and the answer lives
 * here: find the promise covering the faulting address, then fill one
 * frame per its terms — zeros for anonymous spans and the heap, bytes
 * read through the backend for file-backed spans, and the same zeros
 * for the stack, whose bottom page stays unmapped forever as the
 * guard. A fault outside every promise, or against the guard, is a
 * true segmentation fault and is declined; the diagnoser downstream
 * turns the decline into the verdict lines.
 *
 * @author  Charliechen114514
 * @date    2026-10-15
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_mm
 * @copyright Copyright (c) 2026
 */

#pragma once

namespace cinux::mm {

class AddressSpace;
class VmaStore;

namespace demand {

/**
 * @brief         Fill the one page a not-present fault asked for.
 *
 * @param[in]     space        The world the fault happened under.
 * @param[in]     ranges       The register naming the promises.
 * @param[in]     address      CR2, the byte the access touched.
 * @param[in]     error_code   The CPU-pushed fault code.
 * @return        true when the page now stands mapped and the faulting
 *                instruction may re-run; false when the fault belongs
 *                to nobody — outside the register, the guard page, a
 *                kernel-side fault, a protection clash, a refused read,
 *                or no free frame.
 * @note          Declines every protection fault on sight: the CoW
 *                engine owns the repairable slice of those, and what
 *                remains is the register and the table agreeing that
 *                the access may not happen. Also declines an address
 *                whose page already stands mapped — a fill is for
 *                absent pages only.
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       kernel_mm
 */
[[nodiscard]] bool FillPage(const AddressSpace& space, const VmaStore& ranges,
                            unsigned long long address, unsigned long long error_code);

}  // namespace demand

}  // namespace cinux::mm
