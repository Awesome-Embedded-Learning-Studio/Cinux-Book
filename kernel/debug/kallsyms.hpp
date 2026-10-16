/**
 * @file    kallsyms.hpp
 * @brief   The kernel's own name server: addresses in, names out.
 *
 * A sorted table of {address, name} pairs plus a binary search over it.
 * The lookup half knows nothing about where the table came from: tests
 * install hand-built tables through KallsymsInstall, and the real table
 * is generated offline from the linked image and carried in a section of
 * its own — the promise trace.hpp made when it shipped raw addresses.
 * Names print as the linker spelled them (mangled C++ stays mangled);
 * an address below the first symbol or past the table's reach has no
 * name, and the caller is told so rather than handed a neighbor.
 *
 * @author  Charliechen114514
 * @date    2026-10-16
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_debug
 * @copyright Copyright (c) 2026
 */

#pragma once

namespace cinux::debug {

/**
 * @brief         One named address in the kernel's symbol table.
 * @since         0.1.0
 * @ingroup       kernel_debug
 */
struct KallsymEntry {
    unsigned long long addr;  ///< The symbol's linked address.
    const char*        name;  ///< The symbol as the linker spelled it.
};

/**
 * @brief         Replace the table the lookups read.
 * @param[in]     table    Sorted ascending by addr; caller-owned storage
 *                         that must outlive every later lookup.
 * @param[in]     count    Entry count; zero installs an empty table.
 * @return        None.
 * @note          The one seam for every injection source: a host test
 *                hands a hand-built array, the kernel image points this
 *                at its carried-in section. Installing twice simply
 *                replaces; nothing here allocates.
 * @since         0.1.0
 * @ingroup       kernel_debug
 */
void KallsymsInstall(const KallsymEntry* table, unsigned long long count);

/**
 * @brief         Whether a table with at least one entry is installed.
 * @return        True when lookups can name anything.
 * @since         0.1.0
 * @ingroup       kernel_debug
 */
[[nodiscard]] bool KallsymsTableReady();

/**
 * @brief         Name one address.
 * @param[in]     addr            The address to name.
 * @param[out]    symbol_offset   Bytes from the symbol's own address;
 *                                zero on an exact hit.
 * @return        The symbol's name, nullptr when no table is installed
 *                or the address lies below the first symbol.
 * @note          An address past the last symbol still names that last
 *                symbol — data sits above code, and "how far into the
 *                last function" is exactly what a crash wants to say.
 *                The search picks the latest entry at or below addr;
 *                runs on the panic path, so it allocates nothing.
 * @since         0.1.0
 * @ingroup       kernel_debug
 */
[[nodiscard]] const char* KallsymsLookup(unsigned long long  addr,
                                         unsigned long long& symbol_offset);

/**
 * @brief         The table the final link carries in, as the linker spelled it.
 * @note          Declared weak: an image linked without the generated table
 *                (the first link pass, or a host test) leaves both symbols
 *                null and KallsymsInstallBuiltIn quietly does nothing.
 * @since         0.1.0
 * @ingroup       kernel_debug
 */
extern "C" {
// NOLINTNEXTLINE(bugprone-dynamic-static-initializers) a declaration, never initialized here
extern const KallsymEntry       kBuiltInKallsyms[] __attribute__((weak));
// NOLINTNEXTLINE(bugprone-dynamic-static-initializers) a declaration, never initialized here
extern const unsigned long long kBuiltInKallsymsCount __attribute__((weak));
}

/**
 * @brief         Install the built-in table when the image carries one.
 * @return        None.
 * @note          The bring-up seam: call once early so that even the first
 *                assertion or fault dumps names. A no-op in images without
 *                a table.
 * @since         0.1.0
 * @ingroup       kernel_debug
 */
void KallsymsInstallBuiltIn();

}  // namespace cinux::debug
