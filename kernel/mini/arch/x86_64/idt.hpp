/**
 * @file kernel/mini/arch/x86_64/idt.hpp
 * @brief Interrupt Descriptor Table (IDT) - Minimal x86_64 Setup
 *
 * 为 mini kernel 提供最小化的 IDT 配置，只设置必要的异常向量：
 *   - #BP (向量 3)：断点异常，由 INT3 指令触发
 *   - #PF (向量 14)：页错误异常，由缺页触发
 *
 * IDT 在 x86_64 下每个条目 16 字节（128 位），包含：
 *   - 中断处理程序的偏移地址（分为三段存放，共 64 位）
 *   - 代码段选择子（指向 GDT 中的 code segment）
 *   - IST (Interrupt Stack Table) 偏移
 *   - 类型和属性（Present / DPL / Gate Type）
 *
 * 依赖关系：idt_init() 必须在 gdt_init() 之后调用。
 */

#pragma once

#include <stdint.h>

namespace cinux::mini::arch {

// ============================================================
// IDT 常量定义
// ============================================================

/// x86_64 IDT 最大条目数（0-255）
constexpr uint16_t IDT_MAX_ENTRIES = 256;

/// 本阶段需要配置的向量号
constexpr uint8_t IDT_VEC_BP = 3;   ///< 断点异常（Breakpoint）
constexpr uint8_t IDT_VEC_PF = 14;  ///< 页错误异常（Page Fault）

/// IDT 条目类型：64-bit 中断门（无错误码压入，由 ISR 自行处理）
constexpr uint8_t IDT_TYPE_INTERRUPT_GATE = 0x0E;
/// IDT 条目类型：64-bit 陷阱门（与中断门的区别是不清除 IF）
constexpr uint8_t IDT_TYPE_TRAP_GATE = 0x0F;

// ============================================================
// IDT 描述符结构（16 字节）
// ============================================================

/**
 * @brief 64-bit IDT 门描述符
 *
 * 每个 IDT 条目占 16 字节，包含中断处理程序的完整地址、
 * 段选择子、IST 偏移以及类型/权限属性。
 */
struct IdtEntry {
    uint16_t offset_low;    ///< 处理程序地址低 16 位 [0:15]
    uint16_t selector;      ///< 代码段选择子（CS）
    uint8_t  ist;           ///< IST 偏移（0 = 不使用 IST）
    uint8_t  type_attr;     ///< 类型和属性（P | DPL | 0 | Gate Type）
    uint16_t offset_mid;    ///< 处理程序地址中 16 位 [16:31]
    uint32_t offset_high;   ///< 处理程序地址高 32 位 [32:63]
    uint32_t reserved;      ///< 保留，必须为 0
} __attribute__((packed));

/**
 * @brief IDT 寄存器结构（用于 LIDT 指令）
 *
 * 对应 x86 的 LIDT 指令操作数格式：2 字节 limit + 8 字节 base address。
 */
struct IdtPointer {
    uint16_t limit; ///< IDT 字节大小 - 1
    uint64_t base;  ///< IDT 的线性地址
} __attribute__((packed));

// ============================================================
// 中断栈帧结构（由 CPU 在异常/中断时压栈）
// ============================================================

/**
 * @brief x86_64 中断栈帧
 *
 * 当 CPU 响应异常/中断时，会自动将以下寄存器压入栈中。
 * 如果异常产生了错误码（如 #PF），CPU 会在最后额外压入错误码。
 * ISR stub 在跳转到 C 处理函数之前，会将此结构的指针作为第一个参数。
 *
 * 注意：对于没有错误码的异常（如 #BP），ISR stub 需要压入一个
 *       伪错误码（dummy error code = 0）来保持栈帧对齐。
 */
struct InterruptFrame {
    uint64_t r15, r14, r13, r12; ///< 通用寄存器（由 ISR stub 保存）
    uint64_t r11, r10, r9, r8;   ///< 通用寄存器（由 ISR stub 保存）
    uint64_t rdi, rsi, rbp, rdx; ///< 通用寄存器（由 ISR stub 保存）
    uint64_t rcx, rbx, rax;      ///< 通用寄存器（由 ISR stub 保存）
    uint64_t error_code;          ///< 错误码（无错误码的异常由 stub 填 0）
    uint64_t rip;                 ///< 指令指针（由 CPU 压入）
    uint64_t cs;                  ///< 代码段选择子（由 CPU 压入）
    uint64_t rflags;              ///< 标志寄存器（由 CPU 压入）
    uint64_t rsp;                 ///< 栈指针（由 CPU 压入）
    uint64_t ss;                  ///< 栈段选择子（由 CPU 压入）
};

// ============================================================
// 公开接口
// ============================================================

/**
 * @brief 初始化并加载 IDT
 *
 * 清空全部 256 个 IDT 条目，然后配置 #BP(3) 和 #PF(14) 两个异常向量，
 * 最后执行 LIDT 加载。ISR stub 地址来自 interrupts.S 中定义的符号。
 *
 * @note 必须在 gdt_init() 之后调用，因为 IDT 条目中的 selector
 *       引用了 GDT 中的代码段选择子。
 */
void idt_init();

} // namespace cinux::mini::arch
