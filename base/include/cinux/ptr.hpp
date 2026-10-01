/**
 * @file    ptr.hpp
 * @brief   The single choke point that reads an integer address as a pointer.
 *
 * Every world sometimes holds an address as an integer: fixed mailbox
 * slots, type-erased format arguments, physical addresses handed across a
 * world switch. Routing all of those through PtrAt keeps one home for the
 * cast and its lint suppression instead of one per call site.
 *
 * @author  Charliechen114514
 * @date    2026-10-01
 * @version 0.1
 * @since   0.1.0
 * @ingroup base_ptr
 * @copyright Copyright (c) 2026
 */

#pragma once

namespace cinux::base {

/**
 * @brief         Reads an integer address back as a pointer to T.
 *
 * @param[in]     address   Linear address carried as an integer.
 * @return        A pointer to T at that address.
 * @note          The raw bits are the point: format Args store pointers as
 *                words and boot mailboxes hold addresses as constants —
 *                this is where they become pointers again. Constness is
 *                the caller's choice: assign to T const* for a read-only
 *                view, keep T* for writing.
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       base_ptr
 */
template <typename T>
T* PtrAt(unsigned long address) {
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    return reinterpret_cast<T*>(address);
}

}  // namespace cinux::base
