#include "cinux/page/page_entry.hpp"
#include "lm.hpp"

namespace {
using cinux::base::page::Entry;
using cinux::base::page::kLargePageSize;
using cinux::base::page::kWritable;
using cinux::base::page::MakeLargePageEntry;
using cinux::base::page::MakeTableEntry;

using cinux::boot::lm::kPdPhys;
using cinux::boot::lm::kPdptPhys;
using cinux::boot::lm::kPml4Phys;

constexpr unsigned int kEntriesPerPage = 512;

// NOLINTBEGIN(performance-no-int-to-ptr)
Entry* table_at(unsigned long physical) {
    return reinterpret_cast<Entry*>(physical);
}
// NOLINTEND(performance-no-int-to-ptr)
}  // namespace

extern "C" void BuildTemporaryPageTables() {
    Entry* const kPml4 = table_at(kPml4Phys);
    Entry* const kPdpt = table_at(kPdptPhys);
    Entry* const kPd   = table_at(kPdPhys);

    for (unsigned int i = 0; i < kEntriesPerPage; ++i) {
        kPml4[i] = Entry{};
        kPdpt[i] = Entry{};
        kPd[i]   = Entry{};
    }

    kPml4[0] = MakeTableEntry(kPdptPhys, kWritable);
    kPdpt[0] = MakeTableEntry(kPdPhys, kWritable);

    for (unsigned long i = 0; i < 4; ++i) {
        kPd[i] = MakeLargePageEntry(i * kLargePageSize, kWritable);
    }
}
