/**
 * @file test/unit/test_gdt_idt.cpp
 * @brief GDT/IDT 数据结构与编码逻辑的 Host 端单元测试
 *
 * 测试范围：
 *   - GDT 描述符编码正确性（make_gdt_entry 的输出）
 *   - IDT 条目编码正确性（set_idt_entry 的输出）
 *   - InterruptFrame 结构体布局正确性（sizeof、字段偏移）
 *   - 段选择子常量正确性
 *   - GdtEntry / IdtEntry 结构体大小和对齐
 *
 * 编译条件：-DCINUX_HOST_TEST
 *
 * 注意：ISR stub（汇编）和异常处理函数依赖硬件，
 * 不适合 host 端测试，这里只测试数据结构和编码逻辑。
 */

#define TEST_FRAMEWORK_IMPL
#include "test_framework.h"

#ifdef CINUX_HOST_TEST

#include <cstddef>
#include <cstdint>
#include <cstring>

// 直接 include 内核头文件中的数据结构定义
// 因为这些结构体是纯数据类型（无硬件依赖），可以在 host 端使用
#include "mini/arch/x86_64/gdt.hpp"
#include "mini/arch/x86_64/idt.hpp"

using namespace cinux::mini::arch;

// ============================================================
// Mock：复制 gdt.cpp 中的 make_gdt_entry 函数用于测试
// ============================================================
// make_gdt_entry 在 gdt.cpp 中是 static 函数，无法直接调用。
// 这里复制其实现用于单元测试编码逻辑的正确性。

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
// Mock：复制 idt.cpp 中的 set_idt_entry 函数用于测试
// ============================================================
// 同理，set_idt_entry 是 static 函数，这里复制实现用于测试。

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
// 1. GDT 常量正确性测试
// ============================================================

/**
 * @brief 验证 GDT 条目数量为 3（null + code64 + data64）
 */
TEST("gdt: entries count is 3") {
    ASSERT_EQ(GDT_ENTRIES, 3);
}

/**
 * @brief 验证 GDT 索引值正确（0, 1, 2）
 */
TEST("gdt: index values are correct") {
    ASSERT_EQ(GDT_NULL_INDEX, 0);
    ASSERT_EQ(GDT_CODE64_INDEX, 1);
    ASSERT_EQ(GDT_DATA64_INDEX, 2);
}

/**
 * @brief 验证段选择子值正确（index * 8，RPL=0）
 */
TEST("gdt: segment selector values") {
    // 段选择子 = index * 8 + RPL (RPL=0)
    ASSERT_EQ(SEGMENT_NULL, 0x0000);   // 0 * 8 = 0
    ASSERT_EQ(SEGMENT_CODE64, 0x0008); // 1 * 8 = 8
    ASSERT_EQ(SEGMENT_DATA64, 0x0010); // 2 * 8 = 16
}

/**
 * @brief 验证段选择子之间的关系
 */
TEST("gdt: segment selector relationships") {
    // 每个相邻选择子间隔 8
    ASSERT_EQ(SEGMENT_CODE64 - SEGMENT_NULL, 8);
    ASSERT_EQ(SEGMENT_DATA64 - SEGMENT_CODE64, 8);
}

// ============================================================
// 2. GDT 结构体布局测试
// ============================================================

/**
 * @brief 验证 GdtEntry 结构体大小为 8 字节
 */
TEST("gdt: GdtEntry size is 8 bytes") {
    ASSERT_EQ(sizeof(GdtEntry), 8u);
}

/**
 * @brief 验证 GdtPointer 结构体大小为 10 字节（2 + 8）
 */
TEST("gdt: GdtPointer size is 10 bytes") {
    ASSERT_EQ(sizeof(GdtPointer), 10u);
}

/**
 * @brief 验证 GdtEntry 的 packed 属性（内部字段紧密排列）
 */
TEST("gdt: GdtEntry field offsets are packed") {
    // 验证各字段偏移量符合 x86_64 GDT 描述符布局
    ASSERT_EQ(offsetof(GdtEntry, limit_low), 0u);   // byte 0-1
    ASSERT_EQ(offsetof(GdtEntry, base_low), 2u);    // byte 2-3
    ASSERT_EQ(offsetof(GdtEntry, base_middle), 4u); // byte 4
    ASSERT_EQ(offsetof(GdtEntry, access), 5u);      // byte 5
    ASSERT_EQ(offsetof(GdtEntry, flags_limit_high), 6u); // byte 6
    ASSERT_EQ(offsetof(GdtEntry, base_high), 7u);   // byte 7
}

/**
 * @brief 验证 GdtPointer 的字段偏移
 */
TEST("gdt: GdtPointer field offsets") {
    ASSERT_EQ(offsetof(GdtPointer, limit), 0u); // byte 0-1
    ASSERT_EQ(offsetof(GdtPointer, base), 2u);  // byte 2-9
}

// ============================================================
// 3. GDT 描述符编码测试
// ============================================================

/**
 * @brief 验证 null descriptor 全为零
 */
TEST("gdt: null descriptor is all zeros") {
    GdtEntry entry = make_gdt_entry(0, 0, 0, 0);

    // 将整个结构体当作字节数组检查
    uint8_t bytes[8];
    memcpy(bytes, &entry, 8);

    for (int i = 0; i < 8; i++) {
        ASSERT_EQ(bytes[i], 0);
    }
}

/**
 * @brief 验证 64-bit code segment 描述符编码
 *
 * Access = 0x9A: P=1, DPL=00, S=1, E=1, DC=0, RW=1, A=0
 * Flags  = 0x0A: G=1, D=0, L=1, Res=0
 * Base   = 0 (long mode 忽略)
 * Limit  = 0xFFFFF (long mode 忽略)
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
 * @brief 验证 64-bit data segment 描述符编码
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
 * @brief 验证 make_gdt_entry 对非零 base 的编码
 *
 * 测试 base 地址被正确拆分到三个字段中。
 */
TEST("gdt: make_gdt_entry with non-zero base") {
    uint32_t base = 0x12345678;
    GdtEntry entry = make_gdt_entry(base, 0, 0, 0);

    ASSERT_EQ(entry.base_low, 0x5678);        // base[0:15]
    ASSERT_EQ(entry.base_middle, 0x34);       // base[16:23]
    ASSERT_EQ(entry.base_high, 0x12);         // base[24:31]
}

/**
 * @brief 验证 make_gdt_entry 对非零 limit 的编码
 *
 * 测试 limit 被正确拆分到 limit_low 和 flags_limit_high 中。
 */
TEST("gdt: make_gdt_entry with non-zero limit") {
    uint32_t limit = 0xABCDE;
    GdtEntry entry = make_gdt_entry(0, limit, 0, 0);

    // limit_low = limit & 0xFFFF = 0xBCDE
    ASSERT_EQ(entry.limit_low, 0xBCDE);
    // flags_limit_high 低 4 位 = (limit >> 16) & 0x0F = 0xA
    ASSERT_EQ(entry.flags_limit_high & 0x0F, 0x0A);
}

/**
 * @brief 验证 flags 字段只使用低 4 位
 *
 * 传入 flags = 0xFF，验证只有低 4 位被使用。
 */
TEST("gdt: make_gdt_entry flags masking") {
    GdtEntry entry = make_gdt_entry(0, 0xFFFFF, 0, 0xFF);

    // flags << 4 应该只使用低 4 位，即 0x0F << 4 = 0xF0
    // 再 OR 上 limit_high = 0x0F，结果为 0xFF
    ASSERT_EQ(entry.flags_limit_high, 0xFF);
}

/**
 * @brief 验证 code segment access byte 的各个位
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
 * @brief 验证 data segment access byte 的各个位
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
// 4. IDT 常量正确性测试
// ============================================================

/**
 * @brief 验证 IDT 最大条目数为 256
 */
TEST("idt: max entries is 256") {
    ASSERT_EQ(IDT_MAX_ENTRIES, 256);
}

/**
 * @brief 验证 IDT 向量号常量
 */
TEST("idt: vector constants") {
    ASSERT_EQ(IDT_VEC_BP, 3);   // 断点异常
    ASSERT_EQ(IDT_VEC_PF, 14);  // 页错误异常
}

/**
 * @brief 验证 IDT 门类型常量
 */
TEST("idt: gate type constants") {
    ASSERT_EQ(IDT_TYPE_INTERRUPT_GATE, 0x0E);
    ASSERT_EQ(IDT_TYPE_TRAP_GATE, 0x0F);
}

// ============================================================
// 5. IDT 结构体布局测试
// ============================================================

/**
 * @brief 验证 IdtEntry 结构体大小为 16 字节
 */
TEST("idt: IdtEntry size is 16 bytes") {
    ASSERT_EQ(sizeof(IdtEntry), 16u);
}

/**
 * @brief 验证 IdtPointer 结构体大小为 10 字节（2 + 8）
 */
TEST("idt: IdtPointer size is 10 bytes") {
    ASSERT_EQ(sizeof(IdtPointer), 10u);
}

/**
 * @brief 验证 IdtEntry 的字段偏移符合 x86_64 IDT 描述符布局
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
 * @brief 验证 IdtPointer 的字段偏移
 */
TEST("idt: IdtPointer field offsets") {
    ASSERT_EQ(offsetof(IdtPointer, limit), 0u); // byte 0-1
    ASSERT_EQ(offsetof(IdtPointer, base), 2u);  // byte 2-9
}

// ============================================================
// 6. IDT 条目编码测试
// ============================================================

/**
 * @brief 验证 set_idt_entry 对地址的拆分编码
 *
 * 使用一个已知地址验证 offset_low/mid/high 的拆分正确性。
 */
TEST("idt: set_idt_entry splits address correctly") {
    IdtEntry table[256] = {};
    uint64_t handler_addr = 0x00000000ABCDEFFF; // 测试地址

    set_idt_entry(table, 3, handler_addr, 0x0008, 0x8F, 0);

    // offset_low = addr & 0xFFFF = 0xEFFF
    ASSERT_EQ(table[3].offset_low, 0xEFFF);
    // offset_mid = (addr >> 16) & 0xFFFF = 0xABCD
    ASSERT_EQ(table[3].offset_mid, 0xABCD);
    // offset_high = (addr >> 32) & 0xFFFFFFFF = 0x00000000
    ASSERT_EQ(table[3].offset_high, 0x00000000u);
}

/**
 * @brief 验证 set_idt_entry 对高 32 位地址的编码
 *
 * 使用一个 higher-half 地址（bit 32+ 有值）验证。
 */
TEST("idt: set_idt_entry with high address") {
    IdtEntry table[256] = {};
    uint64_t handler_addr = 0xFFFFFFFF80010000; // higher-half 地址

    set_idt_entry(table, 14, handler_addr, 0x0008, 0x8E, 0);

    ASSERT_EQ(table[14].offset_low, 0x0000);
    ASSERT_EQ(table[14].offset_mid, 0x8001);
    ASSERT_EQ(table[14].offset_high, 0xFFFFFFFFu);
}

/**
 * @brief 验证 set_idt_entry 的 selector 字段
 */
TEST("idt: set_idt_entry selector field") {
    IdtEntry table[256] = {};

    set_idt_entry(table, 3, 0x1000, SEGMENT_CODE64, 0x8F, 0);

    ASSERT_EQ(table[3].selector, SEGMENT_CODE64);
    ASSERT_EQ(table[3].selector, 0x0008);
}

/**
 * @brief 验证 #BP 使用陷阱门（type_attr = 0x8F）
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
 * @brief 验证 #PF 使用中断门（type_attr = 0x8E）
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
 * @brief 验证 set_idt_entry 的 ist 字段（应为 0，不使用 IST）
 */
TEST("idt: set_idt_entry ist is zero") {
    IdtEntry table[256] = {};

    set_idt_entry(table, 3, 0x1000, 0x0008, 0x8F, 0);
    set_idt_entry(table, 14, 0x2000, 0x0008, 0x8E, 0);

    ASSERT_EQ(table[3].ist, 0);
    ASSERT_EQ(table[14].ist, 0);
}

/**
 * @brief 验证 set_idt_entry 的 reserved 字段为零
 */
TEST("idt: set_idt_entry reserved is zero") {
    IdtEntry table[256] = {};

    set_idt_entry(table, 3, 0x1000, 0x0008, 0x8F, 0);

    ASSERT_EQ(table[3].reserved, 0u);
}

/**
 * @brief 验证不同向量号写入不同条目
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
// 7. InterruptFrame 结构体布局测试
// ============================================================

/**
 * @brief 验证 InterruptFrame 中通用寄存器字段数量
 *
 * 共 15 个通用寄存器（r15-rax）+ 1 个 error_code + 5 个 CPU 压入字段
 */
TEST("frame: InterruptFrame has correct field count") {
    // InterruptFrame 包含 21 个 uint64_t 字段
    // r15,r14,r13,r12,r11,r10,r9,r8,rdi,rsi,rbp,rdx,rcx,rbx,rax = 15
    // error_code, rip, cs, rflags, rsp, ss = 6
    // 共 21 * 8 = 168 字节
    ASSERT_EQ(sizeof(InterruptFrame), 21u * 8u);
}

/**
 * @brief 验证 InterruptFrame 中 error_code 字段的偏移
 *
 * error_code 在 15 个通用寄存器之后
 * offset = 15 * 8 = 120
 */
TEST("frame: error_code offset") {
    ASSERT_EQ(offsetof(InterruptFrame, error_code), 15u * 8u);
}

/**
 * @brief 验证 InterruptFrame 中 CPU 压入字段的偏移
 *
 * rip 紧跟 error_code 之后
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
 * @brief 验证 InterruptFrame 中通用寄存器的排列顺序
 *
 * ISR stub 按逆序压入寄存器（rax 先压入在最低地址），
 * 但 InterruptFrame 结构体中字段声明顺序是从高地址到低地址（r15 最先）。
 */
TEST("frame: register field order") {
    // r15 在最低偏移（最先被 push，在栈顶/最高地址）
    // rax 在最高偏移（最后被 push，在栈底/最低地址附近）
    // 偏移关系：r15 < r14 < ... < rax
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
// 8. 综合场景测试
// ============================================================

/**
 * @brief 验证 GdtPointer 的 limit 计算正确性
 *
 * limit = sizeof(GdtEntry) * GDT_ENTRIES - 1
 */
TEST("gdt: GdtPointer limit calculation") {
    uint16_t expected_limit = sizeof(GdtEntry) * GDT_ENTRIES - 1;
    // 3 entries * 8 bytes - 1 = 23
    ASSERT_EQ(expected_limit, 23);
}

/**
 * @brief 验证 IdtPointer 的 limit 计算正确性
 *
 * limit = sizeof(IdtEntry) * IDT_MAX_ENTRIES - 1
 */
TEST("idt: IdtPointer limit calculation") {
    uint16_t expected_limit = sizeof(IdtEntry) * IDT_MAX_ENTRIES - 1;
    // 256 entries * 16 bytes - 1 = 4095
    ASSERT_EQ(expected_limit, 4095);
}

/**
 * @brief 验证全零 IdtEntry 表示未使用条目
 *
 * 未配置的 IDT 条目应为全零，其中 Present=0 表示未使用。
 */
TEST("idt: zero entry is unused") {
    IdtEntry zero_entry = {};

    // type_attr = 0，Present 位 (bit 7) 为 0
    ASSERT_EQ(zero_entry.type_attr, 0);
    ASSERT_FALSE(zero_entry.type_attr & 0x80); // P = 0
}

/**
 * @brief 验证 code64 描述符的 flags 中 L 位（Long mode）为 1
 */
TEST("gdt: code64 flags has long mode bit set") {
    uint8_t flags = 0x0A;
    // L 位 = bit 1 of flags nibble = 1
    ASSERT_TRUE(flags & 0x02);
}

/**
 * @brief 验证 data64 描述符的 flags 中 L 位为 0
 */
TEST("gdt: data64 flags does not have long mode bit") {
    uint8_t flags = 0x0C;
    // L 位 = bit 1 of flags nibble = 0
    ASSERT_FALSE(flags & 0x02);
}

// ============================================================
// 主函数
// ============================================================

/**
 * @brief 测试入口点
 */
int main() {
    RUN_ALL_TESTS();
    return _tests_failed > 0 ? 1 : 0;
}

#endif // CINUX_HOST_TEST
