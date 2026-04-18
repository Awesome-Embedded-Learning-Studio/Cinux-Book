/**
 * @file test/unit/test_gdt_idt.cpp
 * @brief Host-side unit tests for GDT/IDT data structures and encoding logic
 *
 * Test coverage:
 *   - GDT descriptor encoding correctness (make_gdt_entry output)
 *   - IDT entry encoding correctness (set_idt_entry output)
 *   - InterruptFrame struct layout correctness (sizeof, field offsets)
 *   - Segment selector constant correctness
 *   - GdtEntry / IdtEntry struct size and alignment
 *
 * Compile condition: -DCINUX_HOST_TEST
 *
 * Note: ISR stubs (assembly) and exception handlers depend on hardware,
 * and are not suitable for host-side testing. Only data structures and
 * encoding logic are tested here.
 */

#define TEST_FRAMEWORK_IMPL
#include "test_framework.h"

#ifdef CINUX_HOST_TEST

#include <cstddef>
#include <cstdint>
#include <cstring>

// Directly include kernel header data structure definitions
// These structs are pure data types (no hardware dependency) and can be used on host side
#include "mini/arch/x86_64/gdt.hpp"
#include "mini/arch/x86_64/idt.hpp"

using namespace cinux::mini::arch;

// ============================================================
// Mock: Copy the make_gdt_entry function from gdt.cpp for testing
// ============================================================
// make_gdt_entry is a static function in gdt.cpp and cannot be called directly.
// Here we copy its implementation for unit testing the encoding logic correctness.

static GdtEntry make_gdt_entry(uint32_t base, uint32_t limit, uint8_t access, uint8_t flags) {
    GdtEntry entry;
    entry.limit_low        = limit & 0xFFFF;
    entry.base_low         = base & 0xFFFF;
    entry.base_middle      = (base >> 16) & 0xFF;
    entry.access           = access;
    entry.flags_limit_high = ((flags & 0x0F) << 4) | ((limit >> 16) & 0x0F);
    entry.base_high        = (base >> 24) & 0xFF;
    return entry;
}

// ============================================================
// Mock: Copy the set_idt_entry function from idt.cpp for testing
// ============================================================
// Similarly, set_idt_entry is a static function; copied here for testing.

static void set_idt_entry(IdtEntry* table, uint8_t vector, uint64_t handler_addr,
                          uint16_t selector, uint8_t type_attr, uint8_t ist) {
    table[vector].offset_low  = handler_addr & 0xFFFF;
    table[vector].offset_mid  = (handler_addr >> 16) & 0xFFFF;
    table[vector].offset_high = (handler_addr >> 32) & 0xFFFFFFFF;

    table[vector].selector   = selector;
    table[vector].ist        = ist;
    table[vector].type_attr  = type_attr;
    table[vector].reserved   = 0;
}

// ============================================================
// 1. GDT constant correctness tests
// ============================================================

/**
 * @brief Verify GDT entry count is 3 (null + code64 + data64)
 */
TEST("gdt: entries count is 3") {
    ASSERT_EQ(GDT_ENTRIES, 3);
}

/**
 * @brief Verify GDT index values are correct (0, 1, 2)
 */
TEST("gdt: index values are correct") {
    ASSERT_EQ(GDT_NULL_INDEX, 0);
    ASSERT_EQ(GDT_CODE64_INDEX, 1);
    ASSERT_EQ(GDT_DATA64_INDEX, 2);
}

/**
 * @brief Verify segment selector values are correct (index * 8, RPL=0)
 */
TEST("gdt: segment selector values") {
    // Segment selector = index * 8 + RPL (RPL=0)
    ASSERT_EQ(SEGMENT_NULL, 0x0000);   // 0 * 8 = 0
    ASSERT_EQ(SEGMENT_CODE64, 0x0008); // 1 * 8 = 8
    ASSERT_EQ(SEGMENT_DATA64, 0x0010); // 2 * 8 = 16
}

/**
 * @brief Verify relationships between segment selectors
 */
TEST("gdt: segment selector relationships") {
    // Each adjacent selector is separated by 8
    ASSERT_EQ(SEGMENT_CODE64 - SEGMENT_NULL, 8);
    ASSERT_EQ(SEGMENT_DATA64 - SEGMENT_CODE64, 8);
}

// ============================================================
// 2. GDT struct layout tests
// ============================================================

/**
 * @brief Verify GdtEntry struct size is 8 bytes
 */
TEST("gdt: GdtEntry size is 8 bytes") {
    ASSERT_EQ(sizeof(GdtEntry), 8u);
}

/**
 * @brief Verify GdtPointer struct size is 10 bytes (2 + 8)
 */
TEST("gdt: GdtPointer size is 10 bytes") {
    ASSERT_EQ(sizeof(GdtPointer), 10u);
}

/**
 * @brief Verify GdtEntry packed attribute (internal fields tightly packed)
 */
TEST("gdt: GdtEntry field offsets are packed") {
    // Verify field offsets conform to x86_64 GDT descriptor layout
    ASSERT_EQ(offsetof(GdtEntry, limit_low), 0u);   // byte 0-1
    ASSERT_EQ(offsetof(GdtEntry, base_low), 2u);    // byte 2-3
    ASSERT_EQ(offsetof(GdtEntry, base_middle), 4u); // byte 4
    ASSERT_EQ(offsetof(GdtEntry, access), 5u);      // byte 5
    ASSERT_EQ(offsetof(GdtEntry, flags_limit_high), 6u); // byte 6
    ASSERT_EQ(offsetof(GdtEntry, base_high), 7u);   // byte 7
}

/**
 * @brief Verify GdtPointer field offsets
 */
TEST("gdt: GdtPointer field offsets") {
    ASSERT_EQ(offsetof(GdtPointer, limit), 0u); // byte 0-1
    ASSERT_EQ(offsetof(GdtPointer, base), 2u);  // byte 2-9
}

// ============================================================
// 3. GDT descriptor encoding tests
// ============================================================

/**
 * @brief Verify null descriptor is all zeros
 */
TEST("gdt: null descriptor is all zeros") {
    GdtEntry entry = make_gdt_entry(0, 0, 0, 0);

    // Treat the entire struct as a byte array for checking
    uint8_t bytes[8];
    memcpy(bytes, &entry, 8);

    for (int i = 0; i < 8; i++) {
        ASSERT_EQ(bytes[i], 0);
    }
}

/**
 * @brief Verify 64-bit code segment descriptor encoding
 *
 * Access = 0x9A: P=1, DPL=00, S=1, E=1, DC=0, RW=1, A=0
 * Flags  = 0x0A: G=1, D=0, L=1, Res=0
 * Base   = 0 (ignored in long mode)
 * Limit  = 0xFFFFF (ignored in long mode)
 */
TEST("gdt: code64 descriptor encoding") {
    GdtEntry entry = make_gdt_entry(0, 0xFFFFF, 0x9A, 0x0A);

    // limit_low = 0xFFFF
    ASSERT_EQ(entry.limit_low, 0xFFFF);
    // base_low = 0
    ASSERT_EQ(entry.base_low, 0);
    // base_middle = 0
    ASSERT_EQ(entry.base_middle, 0);
    // access = 0x9A
    ASSERT_EQ(entry.access, 0x9A);
    // flags_limit_high = (0x0A << 4) | 0x0F = 0xAF
    ASSERT_EQ(entry.flags_limit_high, 0xAF);
    // base_high = 0
    ASSERT_EQ(entry.base_high, 0);
}

/**
 * @brief Verify 64-bit data segment descriptor encoding
 *
 * Access = 0x92: P=1, DPL=00, S=1, E=0, DC=0, RW=1, A=0
 * Flags  = 0x0C: G=1, D/B=1, L=0, Res=0
 */
TEST("gdt: data64 descriptor encoding") {
    GdtEntry entry = make_gdt_entry(0, 0xFFFFF, 0x92, 0x0C);

    // limit_low = 0xFFFF
    ASSERT_EQ(entry.limit_low, 0xFFFF);
    // base_low = 0
    ASSERT_EQ(entry.base_low, 0);
    // base_middle = 0
    ASSERT_EQ(entry.base_middle, 0);
    // access = 0x92
    ASSERT_EQ(entry.access, 0x92);
    // flags_limit_high = (0x0C << 4) | 0x0F = 0xCF
    ASSERT_EQ(entry.flags_limit_high, 0xCF);
    // base_high = 0
    ASSERT_EQ(entry.base_high, 0);
}

/**
 * @brief Verify make_gdt_entry encoding with non-zero base
 *
 * Test that the base address is correctly split into three fields.
 */
TEST("gdt: make_gdt_entry with non-zero base") {
    uint32_t base = 0x12345678;
    GdtEntry entry = make_gdt_entry(base, 0, 0, 0);

    ASSERT_EQ(entry.base_low, 0x5678);        // base[0:15]
    ASSERT_EQ(entry.base_middle, 0x34);       // base[16:23]
    ASSERT_EQ(entry.base_high, 0x12);         // base[24:31]
}

/**
 * @brief Verify make_gdt_entry encoding with non-zero limit
 *
 * Test that the limit is correctly split into limit_low and flags_limit_high.
 */
TEST("gdt: make_gdt_entry with non-zero limit") {
    uint32_t limit = 0xABCDE;
    GdtEntry entry = make_gdt_entry(0, limit, 0, 0);

    // limit_low = limit & 0xFFFF = 0xBCDE
    ASSERT_EQ(entry.limit_low, 0xBCDE);
    // flags_limit_high low 4 bits = (limit >> 16) & 0x0F = 0xA
    ASSERT_EQ(entry.flags_limit_high & 0x0F, 0x0A);
}

/**
 * @brief Verify flags field only uses low 4 bits
 *
 * Pass flags = 0xFF, verify only low 4 bits are used.
 */
TEST("gdt: make_gdt_entry flags masking") {
    GdtEntry entry = make_gdt_entry(0, 0xFFFFF, 0, 0xFF);

    // flags << 4 should only use low 4 bits, i.e., 0x0F << 4 = 0xF0
    // OR with limit_high = 0x0F, result is 0xFF
    ASSERT_EQ(entry.flags_limit_high, 0xFF);
}

/**
 * @brief Verify individual bits of code segment access byte
 */
TEST("gdt: code64 access byte bit breakdown") {
    uint8_t access = 0x9A;

    // P (Present) = bit 7 = 1
    ASSERT_TRUE(access & 0x80);
    // DPL = bits 6-5 = 00 (ring 0)
    ASSERT_EQ((access >> 5) & 0x03, 0);
    // S (Descriptor type) = bit 4 = 1 (code/data)
    ASSERT_TRUE(access & 0x10);
    // E (Executable) = bit 3 = 1 (code segment)
    ASSERT_TRUE(access & 0x08);
    // DC (Direction/Conforming) = bit 2 = 0
    ASSERT_FALSE(access & 0x04);
    // RW (Read/Write) = bit 1 = 1 (readable code)
    ASSERT_TRUE(access & 0x02);
    // A (Accessed) = bit 0 = 0
    ASSERT_FALSE(access & 0x01);
}

/**
 * @brief Verify individual bits of data segment access byte
 */
TEST("gdt: data64 access byte bit breakdown") {
    uint8_t access = 0x92;

    ASSERT_TRUE(access & 0x80);   // P = 1
    ASSERT_EQ((access >> 5) & 0x03, 0); // DPL = 00
    ASSERT_TRUE(access & 0x10);   // S = 1
    ASSERT_FALSE(access & 0x08);  // E = 0 (data segment)
    ASSERT_FALSE(access & 0x04);  // DC = 0
    ASSERT_TRUE(access & 0x02);   // RW = 1 (writable data)
    ASSERT_FALSE(access & 0x01);  // A = 0
}

// ============================================================
// 4. IDT constant correctness tests
// ============================================================

/**
 * @brief Verify IDT maximum entry count is 256
 */
TEST("idt: max entries is 256") {
    ASSERT_EQ(IDT_MAX_ENTRIES, 256);
}

/**
 * @brief Verify IDT vector number constants
 */
TEST("idt: vector constants") {
    ASSERT_EQ(IDT_VEC_BP, 3);   // Breakpoint exception
    ASSERT_EQ(IDT_VEC_PF, 14);  // Page fault exception
}

/**
 * @brief Verify IDT gate type constants
 */
TEST("idt: gate type constants") {
    ASSERT_EQ(IDT_TYPE_INTERRUPT_GATE, 0x0E);
    ASSERT_EQ(IDT_TYPE_TRAP_GATE, 0x0F);
}

// ============================================================
// 5. IDT struct layout tests
// ============================================================

/**
 * @brief Verify IdtEntry struct size is 16 bytes
 */
TEST("idt: IdtEntry size is 16 bytes") {
    ASSERT_EQ(sizeof(IdtEntry), 16u);
}

/**
 * @brief Verify IdtPointer struct size is 10 bytes (2 + 8)
 */
TEST("idt: IdtPointer size is 10 bytes") {
    ASSERT_EQ(sizeof(IdtPointer), 10u);
}

/**
 * @brief Verify IdtEntry field offsets conform to x86_64 IDT descriptor layout
 */
TEST("idt: IdtEntry field offsets") {
    ASSERT_EQ(offsetof(IdtEntry, offset_low), 0u);    // byte 0-1
    ASSERT_EQ(offsetof(IdtEntry, selector), 2u);      // byte 2-3
    ASSERT_EQ(offsetof(IdtEntry, ist), 4u);           // byte 4
    ASSERT_EQ(offsetof(IdtEntry, type_attr), 5u);     // byte 5
    ASSERT_EQ(offsetof(IdtEntry, offset_mid), 6u);    // byte 6-7
    ASSERT_EQ(offsetof(IdtEntry, offset_high), 8u);   // byte 8-11
    ASSERT_EQ(offsetof(IdtEntry, reserved), 12u);     // byte 12-15
}

/**
 * @brief Verify IdtPointer field offsets
 */
TEST("idt: IdtPointer field offsets") {
    ASSERT_EQ(offsetof(IdtPointer, limit), 0u); // byte 0-1
    ASSERT_EQ(offsetof(IdtPointer, base), 2u);  // byte 2-9
}

// ============================================================
// 6. IDT entry encoding tests
// ============================================================

/**
 * @brief Verify set_idt_entry splits address correctly
 *
 * Use a known address to verify offset_low/mid/high splitting correctness.
 */
TEST("idt: set_idt_entry splits address correctly") {
    IdtEntry table[256] = {};
    uint64_t handler_addr = 0x00000000ABCDEFFF; // Test address

    set_idt_entry(table, 3, handler_addr, 0x0008, 0x8F, 0);

    // offset_low = addr & 0xFFFF = 0xEFFF
    ASSERT_EQ(table[3].offset_low, 0xEFFF);
    // offset_mid = (addr >> 16) & 0xFFFF = 0xABCD
    ASSERT_EQ(table[3].offset_mid, 0xABCD);
    // offset_high = (addr >> 32) & 0xFFFFFFFF = 0x00000000
    ASSERT_EQ(table[3].offset_high, 0x00000000u);
}

/**
 * @brief Verify set_idt_entry encoding for high 32-bit addresses
 *
 * Use a higher-half address (bit 32+ has values) for verification.
 */
TEST("idt: set_idt_entry with high address") {
    IdtEntry table[256] = {};
    uint64_t handler_addr = 0xFFFFFFFF80010000; // Higher-half address

    set_idt_entry(table, 14, handler_addr, 0x0008, 0x8E, 0);

    ASSERT_EQ(table[14].offset_low, 0x0000);
    ASSERT_EQ(table[14].offset_mid, 0x8001);
    ASSERT_EQ(table[14].offset_high, 0xFFFFFFFFu);
}

/**
 * @brief Verify set_idt_entry selector field
 */
TEST("idt: set_idt_entry selector field") {
    IdtEntry table[256] = {};

    set_idt_entry(table, 3, 0x1000, SEGMENT_CODE64, 0x8F, 0);

    ASSERT_EQ(table[3].selector, SEGMENT_CODE64);
    ASSERT_EQ(table[3].selector, 0x0008);
}

/**
 * @brief Verify #BP uses trap gate (type_attr = 0x8F)
 */
TEST("idt: breakpoint uses trap gate") {
    IdtEntry table[256] = {};

    set_idt_entry(table, IDT_VEC_BP, 0x1000, SEGMENT_CODE64, 0x8F, 0);

    uint8_t type_attr = table[IDT_VEC_BP].type_attr;
    // P (bit 7) = 1
    ASSERT_TRUE(type_attr & 0x80);
    // DPL (bits 6-5) = 00
    ASSERT_EQ((type_attr >> 5) & 0x03, 0);
    // Gate type (bits 3-0) = 0xF (trap gate)
    ASSERT_EQ(type_attr & 0x0F, IDT_TYPE_TRAP_GATE);
}

/**
 * @brief Verify #PF uses interrupt gate (type_attr = 0x8E)
 */
TEST("idt: pagefault uses interrupt gate") {
    IdtEntry table[256] = {};

    set_idt_entry(table, IDT_VEC_PF, 0x2000, SEGMENT_CODE64, 0x8E, 0);

    uint8_t type_attr = table[IDT_VEC_PF].type_attr;
    // P (bit 7) = 1
    ASSERT_TRUE(type_attr & 0x80);
    // DPL (bits 6-5) = 00
    ASSERT_EQ((type_attr >> 5) & 0x03, 0);
    // Gate type (bits 3-0) = 0xE (interrupt gate)
    ASSERT_EQ(type_attr & 0x0F, IDT_TYPE_INTERRUPT_GATE);
}

/**
 * @brief Verify set_idt_entry ist field (should be 0, IST not used)
 */
TEST("idt: set_idt_entry ist is zero") {
    IdtEntry table[256] = {};

    set_idt_entry(table, 3, 0x1000, 0x0008, 0x8F, 0);
    set_idt_entry(table, 14, 0x2000, 0x0008, 0x8E, 0);

    ASSERT_EQ(table[3].ist, 0);
    ASSERT_EQ(table[14].ist, 0);
}

/**
 * @brief Verify set_idt_entry reserved field is zero
 */
TEST("idt: set_idt_entry reserved is zero") {
    IdtEntry table[256] = {};

    set_idt_entry(table, 3, 0x1000, 0x0008, 0x8F, 0);

    ASSERT_EQ(table[3].reserved, 0u);
}

/**
 * @brief Verify different vector numbers write different entries
 */
TEST("idt: different vectors write different entries") {
    IdtEntry table[256] = {};

    set_idt_entry(table, 3, 0x1000, 0x0008, 0x8F, 0);
    set_idt_entry(table, 14, 0x2000, 0x0008, 0x8E, 0);

    ASSERT_EQ(table[3].offset_low, 0x1000);
    ASSERT_EQ(table[14].offset_low, 0x2000);
    ASSERT_NE(table[3].offset_low, table[14].offset_low);
}

// ============================================================
// 7. InterruptFrame struct layout tests
// ============================================================

/**
 * @brief Verify InterruptFrame general-purpose register field count
 *
 * 15 general-purpose registers (r15-rax) + 1 error_code + 5 CPU-pushed fields
 */
TEST("frame: InterruptFrame has correct field count") {
    // InterruptFrame contains 21 uint64_t fields
    // r15,r14,r13,r12,r11,r10,r9,r8,rdi,rsi,rbp,rdx,rcx,rbx,rax = 15
    // error_code, rip, cs, rflags, rsp, ss = 6
    // Total 21 * 8 = 168 bytes
    ASSERT_EQ(sizeof(InterruptFrame), 21u * 8u);
}

/**
 * @brief Verify InterruptFrame error_code field offset
 *
 * error_code is after 15 general-purpose registers
 * offset = 15 * 8 = 120
 */
TEST("frame: error_code offset") {
    ASSERT_EQ(offsetof(InterruptFrame, error_code), 15u * 8u);
}

/**
 * @brief Verify InterruptFrame CPU-pushed fields offsets
 *
 * rip immediately follows error_code
 */
TEST("frame: CPU-pushed fields offsets") {
    uint64_t base_offset = offsetof(InterruptFrame, error_code) + 8;

    ASSERT_EQ(offsetof(InterruptFrame, rip), base_offset);           // +0
    ASSERT_EQ(offsetof(InterruptFrame, cs), base_offset + 8);        // +8
    ASSERT_EQ(offsetof(InterruptFrame, rflags), base_offset + 16);   // +16
    ASSERT_EQ(offsetof(InterruptFrame, rsp), base_offset + 24);      // +24
    ASSERT_EQ(offsetof(InterruptFrame, ss), base_offset + 32);       // +32
}

/**
 * @brief Verify InterruptFrame general-purpose register ordering
 *
 * ISR stub pushes registers in reverse order (rax pushed first at lowest address),
 * but InterruptFrame struct field declaration order is from high address to low (r15 first).
 */
TEST("frame: register field order") {
    // r15 at lowest offset (pushed first, at stack top/highest address)
    // rax at highest offset (pushed last, near stack bottom/lowest address)
    // Offset relationship: r15 < r14 < ... < rax
    ASSERT_LT(offsetof(InterruptFrame, r15), offsetof(InterruptFrame, r14));
    ASSERT_LT(offsetof(InterruptFrame, r14), offsetof(InterruptFrame, r13));
    ASSERT_LT(offsetof(InterruptFrame, r13), offsetof(InterruptFrame, r12));
    ASSERT_LT(offsetof(InterruptFrame, r12), offsetof(InterruptFrame, r11));
    ASSERT_LT(offsetof(InterruptFrame, r11), offsetof(InterruptFrame, r10));
    ASSERT_LT(offsetof(InterruptFrame, r10), offsetof(InterruptFrame, r9));
    ASSERT_LT(offsetof(InterruptFrame, r9), offsetof(InterruptFrame, r8));
    ASSERT_LT(offsetof(InterruptFrame, r8), offsetof(InterruptFrame, rdi));
    ASSERT_LT(offsetof(InterruptFrame, rdi), offsetof(InterruptFrame, rsi));
    ASSERT_LT(offsetof(InterruptFrame, rsi), offsetof(InterruptFrame, rbp));
    ASSERT_LT(offsetof(InterruptFrame, rbp), offsetof(InterruptFrame, rdx));
    ASSERT_LT(offsetof(InterruptFrame, rdx), offsetof(InterruptFrame, rcx));
    ASSERT_LT(offsetof(InterruptFrame, rcx), offsetof(InterruptFrame, rbx));
    ASSERT_LT(offsetof(InterruptFrame, rbx), offsetof(InterruptFrame, rax));
}

// ============================================================
// 8. Integration scenario tests
// ============================================================

/**
 * @brief Verify GdtPointer limit calculation correctness
 *
 * limit = sizeof(GdtEntry) * GDT_ENTRIES - 1
 */
TEST("gdt: GdtPointer limit calculation") {
    uint16_t expected_limit = sizeof(GdtEntry) * GDT_ENTRIES - 1;
    // 3 entries * 8 bytes - 1 = 23
    ASSERT_EQ(expected_limit, 23);
}

/**
 * @brief Verify IdtPointer limit calculation correctness
 *
 * limit = sizeof(IdtEntry) * IDT_MAX_ENTRIES - 1
 */
TEST("idt: IdtPointer limit calculation") {
    uint16_t expected_limit = sizeof(IdtEntry) * IDT_MAX_ENTRIES - 1;
    // 256 entries * 16 bytes - 1 = 4095
    ASSERT_EQ(expected_limit, 4095);
}

/**
 * @brief Verify all-zero IdtEntry represents an unused entry
 *
 * Unconfigured IDT entries should be all zeros, where Present=0 indicates unused.
 */
TEST("idt: zero entry is unused") {
    IdtEntry zero_entry = {};

    // type_attr = 0, Present bit (bit 7) is 0
    ASSERT_EQ(zero_entry.type_attr, 0);
    ASSERT_FALSE(zero_entry.type_attr & 0x80); // P = 0
}

/**
 * @brief Verify code64 descriptor flags has Long mode bit (L) set to 1
 */
TEST("gdt: code64 flags has long mode bit set") {
    uint8_t flags = 0x0A;
    // L bit = bit 1 of flags nibble = 1
    ASSERT_TRUE(flags & 0x02);
}

/**
 * @brief Verify data64 descriptor flags has L bit set to 0
 */
TEST("gdt: data64 flags does not have long mode bit") {
    uint8_t flags = 0x0C;
    // L bit = bit 1 of flags nibble = 0
    ASSERT_FALSE(flags & 0x02);
}

// ============================================================
// Main function
// ============================================================

/**
 * @brief Test entry point
 */
int main() {
    RUN_ALL_TESTS();
    return _tests_failed > 0 ? 1 : 0;
}

#endif // CINUX_HOST_TEST
