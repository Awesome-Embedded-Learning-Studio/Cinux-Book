/**
 * @file kernel/mini/arch/x86_64/gdt.hpp
 * @brief Global Descriptor Table (GDT) - Minimal x86_64 Setup
 *
 * 为 mini kernel 提供最基本的 GDT 配置：null descriptor、64-bit code segment、
 * 64-bit data segment 三项。x86_64 下分段机制已经被弱化，GDT 主要用于：
 *   - 设置 CS/DS/ES/SS 等段寄存器的基地址和权限
 *   - 为 long mode 提供必要的代码/数据段描述符
 *   - ISR/IRETQ 需要正确的段选择子来恢复上下文
 *
 * 调用顺序：init() 必须在任何中断/异常处理之前完成。
 */

#pragma once

#include <stdint.h>

namespace cinux::mini::arch {

// ============================================================
// GDT 常量定义
// ============================================================

/// GDT 条目数量：null + code64 + data64 = 3
constexpr uint8_t GDT_ENTRIES = 3;

/// GDT 条目索引（同时也是段选择子的 TI=0 部分，单位 8 字节）
constexpr uint8_t GDT_NULL_INDEX  = 0;
constexpr uint8_t GDT_CODE64_INDEX = 1;
constexpr uint8_t GDT_DATA64_INDEX = 2;

/// 段选择子：index * 8 + RPL (Requested Privilege Level)
/// 这里全部使用 RPL=0（ring 0 内核态）
constexpr uint16_t SEGMENT_NULL  = GDT_NULL_INDEX  * 8;
constexpr uint16_t SEGMENT_CODE64 = GDT_CODE64_INDEX * 8;
constexpr uint16_t SEGMENT_DATA64 = GDT_DATA64_INDEX * 8;

// ============================================================
// GDT 描述符结构（10 字节：8 字节描述符 + 2 字节 limit 低 16 位）
// ============================================================

/**
 * @brief 64-bit GDT 描述符（8 字节）
 *
 * x86_64 long mode 下的段描述符格式，与 legacy 32-bit 格式兼容，
 * 但 base 和 limit 字段在 long mode 下被硬件忽略（除了 GS/FS 的 base）。
 * 关键字段是 Access Byte 和 Flags 中的粒度/大小位。
 */
struct GdtEntry {
    uint16_t limit_low;   ///< 段限长低 16 位
    uint16_t base_low;    ///< 基地址低 16 位
    uint8_t  base_middle; ///< 基地址中 8 位
    uint8_t  access;      ///< 访问权限字节（Type + DPL + P 等）
    uint8_t  flags_limit_high; ///< 高 4 位 flags + 低 4 位 limit 高 4 位
    uint8_t  base_high;   ///< 基地址高 8 位
} __attribute__((packed));

/**
 * @brief GDT 寄存器结构（用于 LGDT 指令）
 *
 * 对应 x86 的 LGDT 指令操作数格式：2 字节 limit + 8 字节 base address。
 * limit = GDT 总字节数 - 1。
 */
struct GdtPointer {
    uint16_t limit; ///< GDT 字节大小 - 1
    uint64_t base;  ///< GDT 的线性地址
} __attribute__((packed));

// ============================================================
// 公开接口
// ============================================================

/**
 * @brief 初始化并加载 GDT
 *
 * 填写三项 GDT（null / code64 / data64），构造 GdtPointer，
 * 执行 LGDT 加载，然后刷新所有段寄存器（CS/DS/ES/FS/GS/SS）
 * 使之指向新的 GDT 条目。
 *
 * @note 必须在启用中断之前调用。调用后 CS = SEGMENT_CODE64，
 *       DS/ES/FS/GS/SS = SEGMENT_DATA64。
 */
void gdt_init();

} // namespace cinux::mini::arch
