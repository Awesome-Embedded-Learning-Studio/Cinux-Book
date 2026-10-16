#include "framework.hpp"
#include "kernel/debug/kallsyms.hpp"
#include "test_assert.hpp"

namespace {

using cinux::debug::KallsymEntry;

TEST("kallsyms: without a table every lookup is answered with nullptr") {
    ASSERT_FALSE(cinux::debug::KallsymsTableReady());
    unsigned long long offset = 99;
    ASSERT_NULL(cinux::debug::KallsymsLookup(0xFFFFFFFF80201000ULL, offset));
}

TEST("kallsyms: a missing built-in table keeps the built-in install quiet") {
    cinux::debug::KallsymsInstallBuiltIn();
    ASSERT_FALSE(cinux::debug::KallsymsTableReady());
}

TEST("kallsyms: an exact hit names the symbol with zero offset") {
    const KallsymEntry kTable[] = {
        {.addr = 0x1000, .name = "kernel_start"},
        {.addr = 0x2000, .name = "schedule"},
        {.addr = 0x3000, .name = "kernel_end"},
    };
    cinux::debug::KallsymsInstall(kTable, 3);
    ASSERT_TRUE(cinux::debug::KallsymsTableReady());
    unsigned long long offset = 77;
    const char* const  kName  = cinux::debug::KallsymsLookup(0x2000, offset);
    ASSERT_NOT_NULL(kName);
    ASSERT_TRUE(kName == kTable[1].name);
    ASSERT_EQ(offset, 0ULL);
}

TEST("kallsyms: an address inside a symbol names it and reports the depth") {
    const KallsymEntry kTable[] = {
        {.addr = 0x1000, .name = "kernel_start"},
        {.addr = 0x2000, .name = "schedule"},
        {.addr = 0x3000, .name = "kernel_end"},
    };
    cinux::debug::KallsymsInstall(kTable, 3);
    unsigned long long offset = 0;
    const char* const  kName  = cinux::debug::KallsymsLookup(0x2080, offset);
    ASSERT_TRUE(kName == kTable[1].name);
    ASSERT_EQ(offset, 0x80ULL);
}

TEST("kallsyms: the boundary below the first symbol has no name") {
    const KallsymEntry kTable[] = {{.addr = 0x2000, .name = "schedule"}};
    cinux::debug::KallsymsInstall(kTable, 1);
    unsigned long long offset = 0;
    ASSERT_NULL(cinux::debug::KallsymsLookup(0x1FFF, offset));
    const char* const kAbove = cinux::debug::KallsymsLookup(0x5000, offset);
    ASSERT_TRUE(kAbove == kTable[0].name);
    ASSERT_EQ(offset, 0x3000ULL);
}

TEST("kallsyms: reinstalling replaces the table the lookups read") {
    const KallsymEntry kFirst[]  = {{.addr = 0x1000, .name = "old"}};
    const KallsymEntry kSecond[] = {{.addr = 0x9000, .name = "new"}};
    cinux::debug::KallsymsInstall(kFirst, 1);
    cinux::debug::KallsymsInstall(kSecond, 1);
    unsigned long long offset = 0;
    const char* const  kName  = cinux::debug::KallsymsLookup(0x9001, offset);
    ASSERT_TRUE(kName == kSecond[0].name);
}

TEST("kallsyms: an empty install leaves lookups honest") {
    cinux::debug::KallsymsInstall(nullptr, 0);
    ASSERT_FALSE(cinux::debug::KallsymsTableReady());
    unsigned long long offset = 0;
    ASSERT_NULL(cinux::debug::KallsymsLookup(0x1000, offset));
}

}  // namespace

int main() {
    return cinux::test::RunAll();
}
