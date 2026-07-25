/**
 * @file kernel/lib/kallsyms.cpp
 * @brief Kernel symbol resolution implementation (binary search, IF=0 safe)
 *
 * All formatting is done by hand (no snprintf in the freestanding kernel) and
 * no locks or allocations are used, so these routines are safe from the panic
 * path (IF=0).  Binary search assumes the table is ascending by address, the
 * contract enforced by the build-time nm generator and the test fixture.
 */

#include "kernel/lib/kallsyms.hpp"

namespace cinux::lib {

namespace {

const KallsymEntry* g_entries = nullptr;
size_t              g_count   = 0;

// Append s to [*p, end), stopping at end.  Returns the new write position.
char* append_str(char* p, char* end, const char* s) {
    while (*s != '\0' && p < end) {
        *p++ = *s++;
    }
    return p;
}

// Append v as lowercase hex (no "0x" prefix) to [*p, end).  Returns new pos.
char* append_hex(char* p, char* end, uint64_t v) {
    char tmp[16];
    int  n = 0;
    if (v == 0) {
        tmp[n++] = '0';
    }
    while (v != 0) {
        tmp[n++] = "0123456789abcdef"[v & 0xf];
        v >>= 4;
    }
    while (n > 0 && p < end) {
        *p++ = tmp[--n];
    }
    return p;
}

}  // namespace

void kallsyms_set_table(const KallsymEntry* entries, size_t count) {
    g_entries = entries;
    g_count   = count;
}

bool kallsyms_available() {
    return g_entries != nullptr && g_count > 0;
}

size_t kallsyms_count() {
    return g_count;
}

bool kallsyms_lookup(uint64_t addr, char* buf, size_t len) {
    if (len == 0) {
        return false;
    }
    char* p   = buf;
    char* end = buf + len - 1;  // reserve one byte for the NUL terminator

    if (!kallsyms_available()) {
        p = append_str(p, end, "0x");
        p = append_hex(p, end, addr);
        *p = '\0';
        return false;
    }

    // Binary search for the first entry whose address is strictly greater than
    // addr; lo-1 is then the greatest entry with address <= addr.
    size_t lo = 0;
    size_t hi = g_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (g_entries[mid].addr <= addr) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }

    if (lo == 0) {
        // addr lies before the first symbol: no covering symbol.
        p = append_str(p, end, "0x");
        p = append_hex(p, end, addr);
        *p = '\0';
        return false;
    }

    const KallsymEntry& e   = g_entries[lo - 1];
    const uint64_t      off = addr - e.addr;
    p                        = append_str(p, end, e.name);
    if (off > 0) {
        p = append_str(p, end, "+0x");
        p = append_hex(p, end, off);
    }
    *p = '\0';
    return true;
}

}  // namespace cinux::lib
