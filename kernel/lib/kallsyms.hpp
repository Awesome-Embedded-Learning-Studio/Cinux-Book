/**
 * @file kernel/lib/kallsyms.hpp
 * @brief Kernel symbol resolution (address -> "name+0xoffset")
 *
 * Provides KALLSYMS-style address-to-symbol lookup so that panic dumps and
 * backtraces can show function names instead of bare addresses.  Aligns with
 * Linux's /proc/kallsyms and kallsyms_lookup_name(): a build-time-generated
 * table of {address, name} (function symbols only, ascending by address) is
 * registered once at boot, and lookups binary-search it for the greatest
 * entry whose address is <= the target.
 *
 * The table is injected via kallsyms_set_table(): the production kernel feeds
 * a table embedded by the build system (nm over the linked ELF), and the
 * in-kernel test suite feeds a small fixture.  All routines are safe to call
 * at IF=0 (no locks, no allocation) so they may be used from the panic path.
 *
 * Namespace: cinux::lib
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace cinux::lib {

/// One kernel symbol: a code address and its name.  Tables are sorted by
/// `addr` in ascending order (the contract of kallsyms_set_table).
struct KallsymEntry {
    uint64_t    addr;
    const char* name;
};

/**
 * @brief Register a sorted symbol table for address->name resolution.
 *
 * Must be called once at boot before any lookup.  The table must remain valid
 * for the kernel's lifetime (it is stored by pointer, not copied).  Passing
 * nullptr/0 unregisters the table.
 *
 * @param entries  Ascending-by-addr symbol entries (may be null).
 * @param count    Number of entries.
 */
void kallsyms_set_table(const KallsymEntry* entries, size_t count);

/// @return True iff a non-empty symbol table has been registered.
bool kallsyms_available();

/// @return Number of registered symbols (0 if none).
size_t kallsyms_count();

/**
 * @brief Resolve @p addr to a human-readable symbol string.
 *
 * Writes the greatest covering symbol as "name" (exact) or "name+0xoffset"
 * into @p buf, always NUL-terminated.  If no table is registered or @p addr
 * precedes the first symbol, writes "0xADDR" and returns false so callers can
 * still print the raw address.
 *
 * @param addr  Address to resolve.
 * @param buf   Output buffer.
 * @param len   Buffer capacity (must be > 0).
 * @return True if a covering symbol was found.
 */
bool kallsyms_lookup(uint64_t addr, char* buf, size_t len);

}  // namespace cinux::lib
