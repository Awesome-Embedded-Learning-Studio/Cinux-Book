/**
 * @file test/unit/test_initial_stack.cpp
 * @brief Host unit test for build_initial_stack (Linux x86_64 entry stack)
 *
 * Reads the built buffer back the way a user process / musl would at _start:
 * walk argc, argv[], NULL, envp[], NULL, then the auxv pairs to AT_NULL. Verifies
 * the entry RSP is 16-byte aligned, argv/envp pointers resolve to the right
 * strings, and the musl-required AT_* entries (AT_PHDR/PAGESZ/ENTRY/UID... plus
 * helper-injected AT_RANDOM/AT_EXECFN) carry correct values.
 */

#define TEST_FRAMEWORK_IMPL

#include <stdint.h>
#include <string.h>

#include "kernel/proc/initial_stack.hpp"
#include "test_framework.h"

using namespace cinux::proc;

namespace {

/// Read back buf as a user process at _start would (all pointer values are VAs).
struct Reader {
    const uint8_t* buf;
    uint64_t       cap;
    uint64_t       stack_top;
    uint64_t       bias;  // stack_top - cap
    uint64_t       rsp;   // stack_top - size

    static Reader make(const uint8_t* b, uint64_t c, uint64_t top, uint64_t size) {
        Reader r;
        r.buf       = b;
        r.cap       = c;
        r.stack_top = top;
        r.bias      = top - c;
        r.rsp       = top - size;
        return r;
    }

    uint64_t off(uint64_t va) const { return va - bias; }

    uint64_t u64(uint64_t va) const {
        uint64_t o = off(va);
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i)
            v |= static_cast<uint64_t>(buf[o + i]) << (8 * i);
        return v;
    }

    bool str_eq(uint64_t va, const char* expected) const {
        return strcmp(reinterpret_cast<const char*>(buf + off(va)), expected) == 0;
    }

    bool bytes_eq(uint64_t va, const uint8_t* expected, uint64_t n) const {
        return memcmp(buf + off(va), expected, n) == 0;
    }
};

}  // namespace

TEST("initial_stack: argc/argv/envp layout and 16-byte RSP alignment") {
    const uint64_t CAP = 512;
    const uint64_t TOP = 0x7FFFFF000;  // % 16 == 0
    uint8_t        buf[CAP];

    const char* argv[]  = {"/bin/prog", "arg1"};
    const char* envp[]  = {"PATH=/bin", "HOME=/root", "USER=root"};
    uint8_t     rnd[16] = {0xDE, 0xAD, 0xBE, 0xEF, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};

    AuxEntry aux[] = {{AT_PHDR, 0x400040},
                      {AT_PHNUM, 3},
                      {AT_PHENT, 56},
                      {AT_PAGESZ, 4096},
                      {AT_ENTRY, 0x401000}};

    uint64_t size = build_initial_stack(buf, CAP, TOP, 2, argv, 3, envp, aux, 5, "/bin/prog", rnd);
    ASSERT_TRUE(size > 0);

    Reader r = Reader::make(buf, CAP, TOP, size);
    ASSERT_EQ(r.rsp % 16, 0ULL);  // SysV: process entry RSP 16-byte aligned

    // argc
    uint64_t p = r.rsp;
    ASSERT_EQ(r.u64(p), 2ULL);
    p += 8;
    // argv[0], argv[1], NULL
    ASSERT_TRUE(r.str_eq(r.u64(p), "/bin/prog"));
    p += 8;
    ASSERT_TRUE(r.str_eq(r.u64(p), "arg1"));
    p += 8;
    ASSERT_EQ(r.u64(p), 0ULL);
    p += 8;
    // envp[0..2], NULL
    ASSERT_TRUE(r.str_eq(r.u64(p), "PATH=/bin"));
    p += 8;
    ASSERT_TRUE(r.str_eq(r.u64(p), "HOME=/root"));
    p += 8;
    ASSERT_TRUE(r.str_eq(r.u64(p), "USER=root"));
    p += 8;
    ASSERT_EQ(r.u64(p), 0ULL);
    p += 8;

    // argv pointer values must be plausible user VAs inside the layout region.
    ASSERT_TRUE(r.u64(r.rsp + 8) >= TOP - size && r.u64(r.rsp + 8) < TOP);
}

TEST("initial_stack: auxv entries incl AT_RANDOM/AT_EXECFN, AT_NULL terminator") {
    const uint64_t CAP = 512;
    const uint64_t TOP = 0x7FFFFF000;
    uint8_t        buf[CAP];

    const char* argv[]  = {"prog"};
    const char* envp[]  = {"A=1"};
    uint8_t     rnd[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    AuxEntry    aux[]   = {{AT_PHDR, 0x400040},  {AT_PHNUM, 2},  {AT_PHENT, 56},  {AT_PAGESZ, 4096},
                           {AT_ENTRY, 0x401000}, {AT_UID, 1000}, {AT_EUID, 1000}, {AT_GID, 1000},
                           {AT_EGID, 1000},      {AT_SECURE, 0}};

    uint64_t size = build_initial_stack(buf, CAP, TOP, 1, argv, 1, envp, aux, 10, "/bin/prog", rnd);
    ASSERT_TRUE(size > 0);
    Reader r = Reader::make(buf, CAP, TOP, size);

    // Walk past argc, argv, NULL, envp, NULL to reach the auxv.
    uint64_t p = r.rsp + 8;  // past argc
    p += 8 * 1;              // argv[0]
    p += 8;                  // argv NULL
    p += 8 * 1;              // envp[0]
    p += 8;                  // envp NULL

    // Scan the auxv for the entries we care about; collect into a small map.
    uint64_t found_phdr = 0, found_pagesz = 0, found_entry = 0;
    uint64_t found_uid = 0xFFFFFFFF, found_random_va = 0, found_execfn_va = 0,
             found_platform_va = 0;
    uint64_t saw_null          = 0;
    for (int i = 0; i < 64; ++i) {
        uint64_t type = r.u64(p);
        uint64_t val  = r.u64(p + 8);
        if (type == AT_NULL) {
            saw_null = 1;
            break;
        }
        switch (type) {
        case AT_PHDR:
            found_phdr = val;
            break;
        case AT_PAGESZ:
            found_pagesz = val;
            break;
        case AT_ENTRY:
            found_entry = val;
            break;
        case AT_UID:
            found_uid = val;
            break;
        case AT_RANDOM:
            found_random_va = val;
            break;
        case AT_EXECFN:
            found_execfn_va = val;
            break;
        case AT_PLATFORM:
            found_platform_va = val;
            break;
        }
        p += 16;
    }
    ASSERT_TRUE(saw_null);
    ASSERT_EQ(found_phdr, 0x400040ULL);
    ASSERT_EQ(found_pagesz, 4096ULL);
    ASSERT_EQ(found_entry, 0x401000ULL);
    ASSERT_EQ(found_uid, 1000ULL);

    // AT_RANDOM points at the 16 injected bytes.
    ASSERT_TRUE(found_random_va != 0);
    ASSERT_TRUE(r.bytes_eq(found_random_va, rnd, 16));

    // AT_EXECFN points at the filename string.
    ASSERT_TRUE(found_execfn_va != 0);
    ASSERT_TRUE(r.str_eq(found_execfn_va, "/bin/prog"));

    // F4-B0: AT_PLATFORM points at "x86_64" (glibc arch dispatch / IFUNC gating).
    ASSERT_TRUE(found_platform_va != 0);
    ASSERT_TRUE(r.str_eq(found_platform_va, "x86_64"));
}

TEST("initial_stack: overflow returns 0") {
    const uint64_t CAP = 16;  // impossibly small
    uint8_t        buf[CAP];
    const char*    argv[]  = {"prog"};
    uint8_t        rnd[16] = {0};
    AuxEntry       aux[]   = {{AT_PAGESZ, 4096}};
    uint64_t       size =
        build_initial_stack(buf, CAP, 0x10000, 1, argv, 0, nullptr, aux, 1, "prog", rnd);
    ASSERT_EQ(size, 0ULL);
}

int main() {
    RUN_ALL_TESTS();
    return _tests_failed > 0 ? 1 : 0;
}
