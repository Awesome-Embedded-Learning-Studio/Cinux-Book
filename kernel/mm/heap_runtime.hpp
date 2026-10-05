/**
 * @file    heap_runtime.hpp
 * @brief   The kernel side of the heap: pages in, allocations out.
 *
 * The Heap class knows arithmetic; this face knows pages. BringUpHeap
 * maps the initial span in the heap window and hands it to the
 * allocator, HeapAllocate grows that span through the shared walk when
 * a request does not fit, and the global new and delete route here so
 * ordinary C++ allocation lands on kernel memory. Host tests never see
 * this file — they drive the pure class directly.
 *
 * @author  Charliechen114514
 * @date    2026-10-05
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_mm
 * @copyright Copyright (c) 2026
 */

#pragma once

namespace cinux::mm {

/**
 * @brief         Maps the initial heap span and starts the allocator.
 *
 * @return        true when Heap::self() is ready to serve.
 * @note          Runs after BringUpAddressSpace: the walk needs the
 *                formal layout, and the ledger must be up to feed it.
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       kernel_mm
 */
bool BringUpHeap();

/**
 * @brief         Allocates, growing the span when the request fits
 *                nowhere.
 *
 * @param[in]     bytes   Payload wanted.
 * @return        Pointer into the heap, or nullptr when even growth
 *                failed.
 * @note          Growth maps whole pages above the current top and
 *                coalesces them into the tail; one spare page rides
 *                along so block headers never strand a request.
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       kernel_mm
 */
void* HeapAllocate(unsigned long bytes);

/**
 * @brief         Returns a heap block, coalescing with free neighbors.
 *
 * @param[in]     block   Payload pointer HeapAllocate handed out.
 * @return        true when accepted; false when the pointer is not a
 *                live block (double free included).
 * @note          The verdict stays queryable here; the global delete
 *                turns a false into a panic instead.
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       kernel_mm
 */
bool HeapFree(void* block);

}  // namespace cinux::mm
