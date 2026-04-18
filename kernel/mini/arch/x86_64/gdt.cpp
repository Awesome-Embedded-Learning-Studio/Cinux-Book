/**
 * @file kernel/mini/arch/x86_64/gdt.cpp
 * @brief GDT 初始化与加载实现
 *
 * 为 mini kernel 设置三项 GDT（null / code64 / data64），
 * 使用 LGDT 加载后通过远跳转刷新 CS 并重载所有数据段寄存器。
 */

#include "gdt.hpp"

namespace cinux::mini::arch {

// ============================================================
// 内部状态
// ============================================================

/// GDT 表实例（三项：null / code64 / data64）
static GdtEntry s_gdt[GDT_ENTRIES];

/// GDTR 加载用的指针结构
static GdtPointer s_gdt_pointer;

// ============================================================
// 内部辅助
// ============================================================

/**
 * @brief 构造一个 GDT 描述符条目
 *
 * @param base   段基地址（long mode 下被忽略，填 0 即可）
 * @param limit  段限长（long mode 下被忽略，填 0xFFFFF 即可）
 * @param access 访问权限字节
 * @param flags  高 4 位标志（granularity / size / long mode）
 * @return 填好的 GdtEntry
 *
 * Access Byte 布局：
 *   Bit 7   : P (Present)        - 1 = 段在内存中
 *   Bit 6-5 : DPL (特权级)       - 00 = ring 0
 *   Bit 4   : S (Descriptor)     - 1 = code/data, 0 = system
 *   Bit 3   : E (Executable)     - 1 = code, 0 = data
 *   Bit 2   : DC (Direction/Conforming)
 *   Bit 1   : RW (Read/Write)
 *   Bit 0   : A (Accessed)       - 由 CPU 自动设置
 *
 * Flags 布局：
 *   Bit 3   : G (Granularity)    - 1 = 4KB 粒度
 *   Bit 2   : D/B (Default)      - 0 for 64-bit code
 *   Bit 1   : L (Long mode)      - 1 = 64-bit code segment
 *   Bit 0   : Reserved           - 0
 */
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
// 公开接口实现
// ============================================================

void gdt_init() {
    // Step 1: 填写 null descriptor（索引 0，全零，不可用）
    s_gdt[GDT_NULL_INDEX] = make_gdt_entry(0, 0, 0, 0);

    // Step 2: 填写 64-bit code segment（索引 1）
    // Access: Present=1, DPL=00, S=1, E=1, DC=0, RW=1, A=0 => 0x9A
    // Flags:  G=1, D=0, L=1, Res=0 => 0x0A (long mode code segment)
    s_gdt[GDT_CODE64_INDEX] = make_gdt_entry(0, 0xFFFFF, 0x9A, 0x0A);

    // Step 3: 填写 64-bit data segment（索引 2）
    // Access: Present=1, DPL=00, S=1, E=0, DC=0, RW=1, A=0 => 0x92
    // Flags:  G=1, D/B=1, L=0, Res=0 => 0x0C (data segment)
    s_gdt[GDT_DATA64_INDEX] = make_gdt_entry(0, 0xFFFFF, 0x92, 0x0C);

    // Step 4: 构造 GDTR 指针
    s_gdt_pointer.limit = sizeof(s_gdt) - 1;
    s_gdt_pointer.base  = reinterpret_cast<uint64_t>(&s_gdt);

    // Step 5: 加载 GDTR 并刷新所有段寄存器
    // 内联汇编完成：lgdt -> far jmp (刷新 CS) -> 重载 DS/ES/FS/GS/SS
    __asm__ volatile (
        "lgdt %[gdtr]\n\t"                 // 加载 GDT 寄存器

        // 远跳转刷新 CS 段寄存器
        "pushq %[cs]\n\t"                  // push 新的代码段选择子
        "leaq 1f(%%rip), %%rax\n\t"        // 取标号 1 的地址作为返回点
        "pushq %%rax\n\t"                  // push 返回地址
        "lretq\n\t"                        // far return -> CS 被刷新
        "1:\n\t"

        // 重载数据段寄存器
        "movw %[ds], %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        "movw %%ax, %%ss\n\t"
        :
        : [gdtr] "m" (s_gdt_pointer),     // 内存操作数，lgdt 直接引用
          [cs]   "i" (SEGMENT_CODE64),      // 立即数，代码段选择子
          [ds]   "i" (SEGMENT_DATA64)       // 立即数，数据段选择子
        : "rax", "memory"
    );
}

} // namespace cinux::mini::arch
