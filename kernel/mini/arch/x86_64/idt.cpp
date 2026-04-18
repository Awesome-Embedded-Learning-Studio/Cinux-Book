/**
 * @file kernel/mini/arch/x86_64/idt.cpp
 * @brief IDT 初始化与加载实现
 *
 * 配置 #BP(3) 和 #PF(14) 两个异常向量的 IDT 条目，
 * 并通过 LIDT 指令加载到 CPU。
 */

#include "idt.hpp"
#include "gdt.hpp"

namespace cinux::mini::arch {

// ============================================================
// 内部状态
// ============================================================

/// IDT 表实例（256 个条目，大部分为空）
static IdtEntry s_idt[IDT_MAX_ENTRIES];

/// IDTR 加载用的指针结构
static IdtPointer s_idt_pointer;

// ============================================================
// ISR stub 声明（定义在 interrupts.S 中）
// ============================================================

/// #BP(3) 的 ISR stub 入口
extern "C" void isr_bp_stub();
/// #PF(14) 的 ISR stub 入口
extern "C" void isr_pf_stub();

// ============================================================
// 异常处理函数声明（定义在 exception_handlers.cpp 中）
// ============================================================

extern "C" void handle_bp(InterruptFrame* frame);
extern "C" void handle_pf(InterruptFrame* frame);

// ============================================================
// 内部辅助
// ============================================================

/**
 * @brief 构造一个 IDT 条目
 *
 * @param vector   向量号（0-255）
 * @param handler  中断处理程序的地址
 * @param selector 代码段选择子（通常为 SEGMENT_CODE64）
 * @param type_attr 类型和属性字节
 *        - Bit 7:   P (Present) = 1
 *        - Bit 6-5: DPL (Descriptor Privilege Level) = 00 (ring 0)
 *        - Bit 4:   0 (固定)
 *        - Bit 3-0: Gate Type (0xE = interrupt, 0xF = trap)
 * @param ist IST 偏移（0 = 不使用 IST）
 */
static void set_idt_entry(uint8_t vector, void* handler, uint16_t selector,
                          uint8_t type_attr, uint8_t ist) {
    uint64_t addr = reinterpret_cast<uint64_t>(handler);

    s_idt[vector].offset_low  = addr & 0xFFFF;
    s_idt[vector].offset_mid  = (addr >> 16) & 0xFFFF;
    s_idt[vector].offset_high = (addr >> 32) & 0xFFFFFFFF;

    s_idt[vector].selector   = selector;
    s_idt[vector].ist        = ist;
    s_idt[vector].type_attr  = type_attr;
    s_idt[vector].reserved   = 0;
}

// ============================================================
// 公开接口实现
// ============================================================

void idt_init() {
    // Step 1: 清空全部 IDT 条目（全部置零，Present=0 表示未使用）
    for (uint16_t i = 0; i < IDT_MAX_ENTRIES; i++) {
        s_idt[i] = IdtEntry{};
    }

    // Step 2: 配置 #BP(3) - 断点异常
    // 使用陷阱门（Trap Gate），这样进入处理程序时 IF 不被清除，
    // 允许在断点处理期间响应其他中断
    // DPL = 3 允许用户态的 INT3 触发（虽然当前我们只有 ring 0）
    // type_attr = 0x8F: P=1, DPL=00, Type=0xF (trap gate)
    set_idt_entry(IDT_VEC_BP,
                  reinterpret_cast<void*>(isr_bp_stub),
                  SEGMENT_CODE64,
                  0x8F,  // Present | DPL=0 | Trap Gate
                  0);    // 不使用 IST

    // Step 3: 配置 #PF(14) - 页错误异常
    // 使用中断门（Interrupt Gate），进入时清除 IF
    // #PF 会自动压入错误码，ISR stub 需要处理
    // type_attr = 0x8E: P=1, DPL=00, Type=0xE (interrupt gate)
    set_idt_entry(IDT_VEC_PF,
                  reinterpret_cast<void*>(isr_pf_stub),
                  SEGMENT_CODE64,
                  0x8E,  // Present | DPL=0 | Interrupt Gate
                  0);    // 不使用 IST

    // Step 4: 构造 IDTR 指针
    s_idt_pointer.limit = static_cast<uint16_t>(sizeof(s_idt) - 1);
    s_idt_pointer.base  = reinterpret_cast<uint64_t>(&s_idt);

    // Step 5: 加载 IDTR
    __asm__ volatile (
        "lidt %[idtr]\n\t"
        :
        : [idtr] "m" (s_idt_pointer)
        : "memory"
    );
}

} // namespace cinux::mini::arch
