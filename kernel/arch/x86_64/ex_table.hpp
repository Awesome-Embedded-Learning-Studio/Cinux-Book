/**
 * @file    ex_table.hpp
 * @brief   The exception table: where a kernel fault may apologize.
 *
 * Some kernel instructions knowingly touch memory that may not answer —
 * the user-access copies above all. Each such instruction plants one
 * entry here naming itself and the landing word that answers for it.
 * When the page-fault path meets a kernel fault, it asks this table
 * first: a hit rewrites the frame's rip to the landing word and the
 * copy reports failure instead of dying. The table's walk is pure
 * memory work, so both worlds share it; only the boot sort and the
 * linker symbols stay kernel-side.
 *
 * @author  Charliechen114514
 * @date    2026-10-23
 * @version 0.3.0
 * @since   0.3.0
 * @ingroup kernel_arch
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <algorithm>

namespace cinux::arch::ex_table {

/// One apology: the instruction that may fault, and the word it lands
/// on when it does. Sixteen bytes, both halves absolute kernel
/// addresses.
struct Entry {
    /// The faulting instruction's address.
    unsigned long long fault_from{};

    /// Where rip should continue instead.
    unsigned long long land_at{};
};

/**
 * @brief         Orders a table by faulting address, in place.
 *
 * @param[in,out] table   The entries to order; caller owns them.
 * @param[in]     count   How many entries the table holds.
 * @return        None.
 * @note          The linker emits entries in translation-unit order,
 *                not address order, and the walk below is a binary
 *                search, so the boot sorts once. The table holds a
 *                handful of entries and the sort runs one time, so
 *                the library sort answers and no hand-rolled ladder
 *                lives here.
 * @since         0.3.0
 * @ingroup       kernel_arch
 */
inline void SortEntries(Entry* table, unsigned long count) {
    std::ranges::sort(table, table + count, {}, &Entry::fault_from);
}

/**
 * @brief         Finds the landing word for one faulting address.
 *
 * @param[in]     table       An address-ordered table, as SortEntries
 *                            leaves it.
 * @param[in]     count       How many entries the table holds.
 * @param[in]     fault_from  The rip the fault stopped at.
 * @return        The landing address, or zero when the table has
 *                nothing to say about this rip.
 * @note          Binary search over the ordered table. Zero as the
 *                miss word is safe: no landing word may legitimately
 *                be zero, because every landing word is kernel text.
 * @since         0.3.0
 * @ingroup       kernel_arch
 */
[[nodiscard]] inline unsigned long long FindFixup(const Entry* table, unsigned long count,
                                                  unsigned long long fault_from) {
    const Entry* const kFound =
        std::ranges::lower_bound(table, table + count, fault_from, {}, &Entry::fault_from);
    if (kFound != table + count && kFound->fault_from == fault_from) {
        return kFound->land_at;
    }
    return 0;
}

/**
 * @brief         Sorts the linked exception table once, at boot.
 *
 * @return        None.
 * @note          Reads the linker's table bounds and orders the real
 *                entries in place. Every world that lets the
 *                user-access copies fault through this table — the
 *                product kernel and each test kernel — calls this
 *                once before any copy runs.
 * @since         0.3.0
 * @ingroup       kernel_arch
 */
void ArmTable();

/**
 * @brief         Asks the linked table about one faulting address.
 *
 * @param[in]     fault_from  The rip the fault stopped at.
 * @return        The landing address, or zero when the table has
 *                nothing to say about this rip.
 * @note          The walk over the real, linker-named table; callers
 *                never touch the section symbols themselves.
 * @since         0.3.0
 * @ingroup       kernel_arch
 */
[[nodiscard]] unsigned long long FindFixupFor(unsigned long long fault_from);

}  // namespace cinux::arch::ex_table
