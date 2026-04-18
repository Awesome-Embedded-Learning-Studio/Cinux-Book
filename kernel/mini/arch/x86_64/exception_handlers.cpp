/**
 * @file kernel/mini/arch/x86_64/exception_handlers.cpp
 * @brief x86_64 异常处理函数实现
 *
 * 提供 #BP(3) 和 #PF(14) 两个异常的 C 语言处理函数。
 * 这些函数由 interrupts.S 中的 ISR stub 调用，
 * 接收 InterruptFrame* 指针作为参数，可以读取/修改中断时的寄存器状态。
 *
 * 当前实现策略：通过串口打印异常信息后继续执行（不断死机），
 * 符合 milestone 目标"触发异常不死机，能看到错误信息"。
 */

#include "idt.hpp"
#include "lib/kprintf.h"

namespace {

using cinux::mini::arch::InterruptFrame;
using cinux::mini::lib::kprintf;

// ============================================================
// 辅助函数：打印 InterruptFrame 中的关键寄存器
// ============================================================

/**
 * @brief 打印中断栈帧中的寄存器快照
 *
 * @param frame 指向中断栈帧的指针
 * @param vec_name 异常名称字符串（如 "#BP", "#PF"）
 * @param vector 向量号
 */
void dump_interrupt_frame(const InterruptFrame* frame, const char* vec_name, uint8_t vector) {
    kprintf("\n");
    kprintf("==== EXCEPTION: %s (vector %u) ====\n", vec_name, vector);
    kprintf("  RIP   = 0x%016x   CS  = 0x%04x\n", frame->rip, frame->cs);
    kprintf("  RFLAGS= 0x%016x\n", frame->rflags);
    kprintf("  RSP   = 0x%016x   SS  = 0x%04x\n", frame->rsp, frame->ss);
    kprintf("  RAX=0x%016x  RBX=0x%016x\n", frame->rax, frame->rbx);
    kprintf("  RCX=0x%016x  RDX=0x%016x\n", frame->rcx, frame->rdx);
    kprintf("  RSI=0x%016x  RDI=0x%016x\n", frame->rsi, frame->rdi);
    kprintf("  RBP=0x%016x  R8 =0x%016x\n", frame->rbp, frame->r8);
    kprintf("  R9 =0x%016x  R10=0x%016x\n", frame->r9, frame->r10);
    kprintf("  R11=0x%016x  R12=0x%016x\n", frame->r11, frame->r12);
    kprintf("  R13=0x%016x  R14=0x%016x\n", frame->r13, frame->r14);
    kprintf("  R15=0x%016x\n", frame->r15);
    kprintf("  ERROR CODE = 0x%016x\n", frame->error_code);
    kprintf("========================================\n");
}

} // anonymous namespace

// ============================================================
// 公开接口（extern "C"，由 interrupts.S 调用）
// ============================================================

/**
 * @brief #BP(3) 断点异常处理函数
 *
 * 当执行 INT3 指令（opcode 0xCC）或 asm volatile("int $3") 时触发。
 * 这是一个陷阱异常，触发时 RIP 指向 INT3 指令的下一条指令，
 * 因此 IRETQ 返回后会继续执行后续代码。
 *
 * @param frame 指向中断栈帧的指针，包含异常发生时的完整寄存器快照
 */
extern "C" void handle_bp(InterruptFrame* frame) {
    // Step 1: 打印断点异常信息
    dump_interrupt_frame(frame, "#BP", 3);

    // Step 2: 打印提示信息，告知这是软件断点，可以安全继续
    kprintf("[EXCEPTION] Breakpoint triggered at RIP=0x%x\n", frame->rip);
    kprintf("[EXCEPTION] This is a software breakpoint, continuing...\n");
}

/**
 * @brief #PF(14) 页错误异常处理函数
 *
 * 当 CPU 尝试访问一个不存在或权限不足的页表项时触发。
 * 触发时 CR2 寄存器保存了导致缺页的线性地址。
 * 错误码格式：
 *   Bit 0 (P):   0=页不存在, 1=权限冲突
 *   Bit 1 (W/R): 0=读, 1=写
 *   Bit 2 (U/S): 0=内核态, 1=用户态
 *   Bit 3 (RSVD):1=保留位冲突
 *   Bit 4 (I/D): 1=取指（指令缺页）
 *
 * @param frame 指向中断栈帧的指针，frame->error_code 包含页错误码
 *
 * @note 当前实现只打印信息并继续，不做任何页修复。
 *       未来加入 VMM 后，这里会变成缺页处理的核心。
 */
extern "C" void handle_pf(InterruptFrame* frame) {
    // Step 1: 读取 CR2 获取导致缺页的地址
    uint64_t fault_addr;
    __asm__ volatile ("movq %%cr2, %0" : "=r"(fault_addr));

    // Step 2: 解析错误码
    uint64_t err = frame->error_code;
    const char* present  = (err & 0x01) ? "protection violation" : "page not present";
    const char* access   = (err & 0x02) ? "write" : "read";
    const char* mode     = (err & 0x04) ? "user" : "kernel";
    const char* reserved = (err & 0x08) ? ", reserved bits" : "";
    const char* fetch    = (err & 0x10) ? ", instruction fetch" : "";

    // Step 3: 打印详细的页错误信息
    dump_interrupt_frame(frame, "#PF", 14);
    kprintf("[EXCEPTION] Page Fault: %s %s %s%s%s\n",
            present, access, mode, reserved, fetch);
    kprintf("[EXCEPTION] Faulting address (CR2) = 0x%016x\n", fault_addr);
    kprintf("[EXCEPTION] Continuing execution...\n");
}
