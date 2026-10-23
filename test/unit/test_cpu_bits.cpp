#include <stdint.h>

#include "../framework/framework.hpp"
#include "kernel/arch/x86_64/page_entry.hpp"
#include "kernel/arch/x86_64/registers.hpp"
#include "test_assert.hpp"

TEST("cpu bits: the no-execute flag is bit sixty-three of an entry") {
    const cinux::arch::page::Entry kFlag = cinux::arch::page::kNoExec;
    ASSERT_EQ(kFlag.raw, 1ULL << 63U);
    ASSERT_TRUE(kFlag.has(kFlag));
}

TEST("cpu bits: a table entry carrying no-execute keeps its frame field") {
    const cinux::arch::page::Entry kEntry =
        cinux::arch::page::MakeTableEntry(0x0012'3000ULL, cinux::arch::page::kNoExec);
    ASSERT_TRUE(kEntry.has(cinux::arch::page::kPresent));
    ASSERT_TRUE(kEntry.has(cinux::arch::page::kNoExec));
    ASSERT_FALSE(kEntry.has(cinux::arch::page::kWritable));
    ASSERT_EQ(kEntry.extract(cinux::arch::page::kTablePhys), 0x12'3000ULL >> 12U);
}

TEST("cpu bits: a large-page entry can refuse execution too") {
    const cinux::arch::page::Entry kEntry = cinux::arch::page::MakeLargePageEntry(
        0x0040'0000ULL, cinux::arch::page::kUser | cinux::arch::page::kNoExec);
    ASSERT_TRUE(kEntry.has(cinux::arch::page::kLarge));
    ASSERT_TRUE(kEntry.has(cinux::arch::page::kUser));
    ASSERT_TRUE(kEntry.has(cinux::arch::page::kNoExec));

    const cinux::arch::page::Entry kPlain =
        cinux::arch::page::MakeLargePageEntry(0x0040'0000ULL, cinux::arch::page::kUser);
    ASSERT_EQ(kPlain.raw >> 63U, 0ULL);
}

TEST("cpu bits: the smep and smap switch positions in cr4") {
    ASSERT_EQ(cinux::arch::kCr4Smep, 1ULL << 20U);
    ASSERT_EQ(cinux::arch::kCr4Smap, 1ULL << 21U);
    ASSERT_EQ(cinux::arch::kCr4Smep & cinux::arch::kCr4Smap, 0ULL);
}

TEST("cpu bits: user leaf terms name exactly the promised access") {
    using cinux::arch::page::Entry;
    using cinux::arch::page::UserLeafTerms;

    const Entry kCode = UserLeafTerms(false, true);
    ASSERT_TRUE(kCode.has(cinux::arch::page::kUser));
    ASSERT_FALSE(kCode.has(cinux::arch::page::kWritable));
    ASSERT_FALSE(kCode.has(cinux::arch::page::kNoExec));

    const Entry kData = UserLeafTerms(true, false);
    ASSERT_TRUE(kData.has(cinux::arch::page::kUser));
    ASSERT_TRUE(kData.has(cinux::arch::page::kWritable));
    ASSERT_TRUE(kData.has(cinux::arch::page::kNoExec));
}

TEST("cpu bits: user leaf terms keep write and execute independent") {
    using cinux::arch::page::UserLeafTerms;

    const cinux::arch::page::Entry kWritableCode = UserLeafTerms(true, true);
    ASSERT_TRUE(kWritableCode.has(cinux::arch::page::kWritable));
    ASSERT_FALSE(kWritableCode.has(cinux::arch::page::kNoExec));

    const cinux::arch::page::Entry kReadOnlyData = UserLeafTerms(false, false);
    ASSERT_FALSE(kReadOnlyData.has(cinux::arch::page::kWritable));
    ASSERT_TRUE(kReadOnlyData.has(cinux::arch::page::kNoExec));
}

int main() {
    return cinux::test::RunAll();
}
