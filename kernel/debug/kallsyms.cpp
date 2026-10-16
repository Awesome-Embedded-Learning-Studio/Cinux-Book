#include "kernel/debug/kallsyms.hpp"

namespace cinux::debug {

namespace {

const KallsymEntry* g_table      = nullptr;
unsigned long long  g_table_size = 0;

unsigned long long upper_bound(unsigned long long addr) {
    unsigned long long low  = 0;
    unsigned long long high = g_table_size;
    while (low < high) {
        const unsigned long long kMid   = low + ((high - low) / 2);
        const bool               kAhead = g_table[kMid].addr <= addr;
        if (kAhead) {
            low = kMid + 1;
        } else {
            high = kMid;
        }
    }
    return low;
}

}  // namespace

void KallsymsInstall(const KallsymEntry* table, unsigned long long count) {
    g_table      = table;
    g_table_size = count;
}

void KallsymsInstallBuiltIn() {
    if (&kBuiltInKallsyms == nullptr || kBuiltInKallsymsCount == 0) {
        return;
    }
    KallsymsInstall(kBuiltInKallsyms, kBuiltInKallsymsCount);
}

bool KallsymsTableReady() {
    return g_table != nullptr && g_table_size != 0;
}

const char* KallsymsLookup(unsigned long long addr, unsigned long long& symbol_offset) {
    if (!KallsymsTableReady() || addr < g_table[0].addr) {
        return nullptr;
    }
    const unsigned long long kSlot = upper_bound(addr);
    const KallsymEntry&      k_hit = g_table[kSlot - 1];
    symbol_offset                  = addr - k_hit.addr;
    return k_hit.name;
}

}  // namespace cinux::debug
