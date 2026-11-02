/**
 * @file    name_table.hpp
 * @brief   A constant table of named entries, found by name.
 *
 * The fixed-name registries keep appearing — a file system's special
 * names, a dispatch table's tags — and each one kept re-rolling the
 * same ladder of string comparisons. This is that ladder, once, as a
 * header-inline constant-expression finder over any array whose entry
 * type carries a NUL-terminated name. The table stays a plain array so
 * it constant-initializes where it is declared; the finder borrows it,
 * trades nothing away, and stays honest with a miss of nullptr.
 *
 * @author  Charliechen114514
 * @date    2026-10-21
 * @version 0.1
 * @since   0.6.0
 * @ingroup base_container
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <algorithm>
#include <iterator>
#include <string_view>

namespace cinux::base::container {

/**
 * @brief         Finds one entry by its name.
 * @tparam        Entry   Entry type; must expose `name` as a
 *                        NUL-terminated C string or char array.
 * @tparam        Count   The array's element count, deduced.
 * @param[in]     table   The constant table to walk.
 * @param[in]     name    The name to find.
 * @return        The matching entry, or nullptr when no name matches.
 * @note          Linear in declaration order; the tables this serves
 *                are a handful of entries, so order is documentation,
 *                not a protocol. Compile-time usable.
 * @since         0.6.0
 * @ingroup       base_container
 */
template <typename Entry, unsigned long Count>
[[nodiscard]] constexpr const Entry* FindByName(const Entry (&table)[Count],
                                                std::string_view name) {
    const auto* const kFound = std::ranges::find_if(
        table, [name](const Entry& entry) { return std::string_view(entry.name) == name; });
    return kFound == std::end(table) ? nullptr : kFound;
}

/**
 * @brief         Finds one entry by its word key.
 * @tparam        Entry   Entry type; must expose `word` as an unsigned
 *                        integer member.
 * @tparam        Count   The array's element count, deduced.
 * @param[in]     table   The constant table to walk.
 * @param[in]     word    The key to find.
 * @return        The matching entry, or nullptr when no key matches.
 * @note          The question-word ladders this serves — an ioctl's
 *                vocabulary, a protocol's tags — are the integer twin
 *                of the name tables above. Compile-time usable.
 * @since         0.6.0
 * @ingroup       base_container
 */
template <typename Entry, unsigned long Count>
[[nodiscard]] constexpr const Entry* FindByWord(const Entry (&table)[Count],
                                                unsigned long long word) {
    const auto* const kFound = std::ranges::find(table, word, &Entry::word);
    return kFound == std::end(table) ? nullptr : kFound;
}

}  // namespace cinux::base::container
