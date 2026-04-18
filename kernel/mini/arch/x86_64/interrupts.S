/**
 * @file kernel/mini/arch/x86_64/interrupts.S
 * @brief ISR (Interrupt Service Routine) Stub 汇编实现
 *
 * 提供 #BP(3) 和 #PF(14) 两个异常的 ISR stub。
 *
 * 每个 stub 的工作流程：
 *   1. 保存所有通用寄存器到栈上（构造 InterruptFrame）
 *   2. 对于没有硬件错误码的异常（#BP），压入伪错误码 0 保持栈对齐
 *   3. 对于有硬件错误码的异常（#PF），CPU 已自动压入，无需额外处理
 *   4. 将当前栈指针（指向 InterruptFrame）作为第一个参数传给 C 处理函数
 *   5. 调用 C 处理函数 (handle_bp / handle_pf)
 *   6. 从函数返回后，恢复所有通用寄存器
 *   7. 通过 IRETQ 返回被中断的代码
 *
 * AT&T 语法说明：
 *   操作数顺序：源操作数, 目标操作数
 *   寄存器前缀 %，立即数前缀 $
 *   内存寻址 offset(base, index, scale)
 */

.section .text

# ============================================================
# ISR Stub 宏：无硬件错误码的异常
# ============================================================
# 对于 #BP 等不产生错误码的异常，需要手动压入伪错误码 0
# 以保持栈帧结构统一，让 C 处理函数能正确解析 InterruptFrame
# ============================================================

.macro ISR_NOERRCODE name vector handler
.global \name
.type \name, @function
\name:
    /* Step 1: 压入伪错误码 0，保持栈对齐 */
    pushq $0                          # $0→(%rsp): 压入伪错误码 0

    /* Step 2: 保存所有通用寄存器 */
    pushq %rax                        # %rax→(%rsp): 保存 RAX
    pushq %rbx                        # %rbx→(%rsp): 保存 RBX
    pushq %rcx                        # %rcx→(%rsp): 保存 RCX
    pushq %rdx                        # %rdx→(%rsp): 保存 RDX
    pushq %rbp                        # %rbp→(%rsp): 保存 RBP
    pushq %rsi                        # %rsi→(%rsp): 保存 RSI
    pushq %rdi                        # %rdi→(%rsp): 保存 RDI
    pushq %r8                         # %r8→(%rsp): 保存 R8
    pushq %r9                         # %r9→(%rsp): 保存 R9
    pushq %r10                        # %r10→(%rsp): 保存 R10
    pushq %r11                        # %r11→(%rsp): 保存 R11
    pushq %r12                        # %r12→(%rsp): 保存 R12
    pushq %r13                        # %r13→(%rsp): 保存 R13
    pushq %r14                        # %r14→(%rsp): 保存 R14
    pushq %r15                        # %r15→(%rsp): 保存 R15

    /* Step 3: 传递 InterruptFrame* 作为第一个参数 */
    movq %rsp, %rdi                   # %rsp→%rdi: 栈指针作为第一个参数

    /* Step 4: 调用 C 处理函数 */
    call \handler                     # 调用对应的 C 异常处理函数

    /* Step 5: 恢复所有通用寄存器 */
    popq %r15                         # (%rsp)→%r15: 恢复 R15
    popq %r14                         # (%rsp)→%r14: 恢复 R14
    popq %r13                         # (%rsp)→%r13: 恢复 R13
    popq %r12                         # (%rsp)→%r12: 恢复 R12
    popq %r11                         # (%rsp)→%r11: 恢复 R11
    popq %r10                         # (%rsp)→%r10: 恢复 R10
    popq %r9                          # (%rsp)→%r9:  恢复 R9
    popq %r8                          # (%rsp)→%r8:  恢复 R8
    popq %rdi                         # (%rsp)→%rdi: 恢复 RDI
    popq %rsi                         # (%rsp)→%rsi: 恢复 RSI
    popq %rbp                         # (%rsp)→%rbp: 恢复 RBP
    popq %rdx                         # (%rsp)→%rdx: 恢复 RDX
    popq %rcx                         # (%rsp)→%rcx: 恢复 RCX
    popq %rbx                         # (%rsp)→%rbx: 恢复 RBX
    popq %rax                         # (%rsp)→%rax: 恢复 RAX

    /* Step 6: 弹出伪错误码 */
    addq $8, %rsp                     # $8→%rsp: 跳过伪错误码

    /* Step 7: 中断返回 */
    iretq                             # 中断返回，恢复 RIP/CS/RFLAGS/RSP/SS
.endm

# ============================================================
# ISR Stub 宏：有硬件错误码的异常
# ============================================================
# 对于 #PF 等会产生错误码的异常，CPU 在进入 ISR 之前已经自动
# 压入了错误码，所以不需要手动压入伪错误码
# ============================================================

.macro ISR_ERRCODE name vector handler
.global \name
.type \name, @function
\name:
    /* Step 1: 保存所有通用寄存器（错误码已在栈上） */

    pushq %rax                        # %rax→(%rsp): 保存 RAX
    pushq %rbx                        # %rbx→(%rsp): 保存 RBX
    pushq %rcx                        # %rcx→(%rsp): 保存 RCX
    pushq %rdx                        # %rdx→(%rsp): 保存 RDX
    pushq %rbp                        # %rbp→(%rsp): 保存 RBP
    pushq %rsi                        # %rsi→(%rsp): 保存 RSI
    pushq %rdi                        # %rdi→(%rsp): 保存 RDI
    pushq %r8                         # %r8→(%rsp): 保存 R8
    pushq %r9                         # %r9→(%rsp): 保存 R9
    pushq %r10                        # %r10→(%rsp): 保存 R10
    pushq %r11                        # %r11→(%rsp): 保存 R11
    pushq %r12                        # %r12→(%rsp): 保存 R12
    pushq %r13                        # %r13→(%rsp): 保存 R13
    pushq %r14                        # %r14→(%rsp): 保存 R14
    pushq %r15                        # %r15→(%rsp): 保存 R15

    /* Step 2: 传递 InterruptFrame* 作为第一个参数 */
    movq %rsp, %rdi                   # %rsp→%rdi: 栈指针作为第一个参数

    /* Step 3: 调用 C 处理函数 */
    call \handler                     # 调用对应的 C 异常处理函数

    /* Step 4: 恢复所有通用寄存器 */
    popq %r15                         # (%rsp)→%r15: 恢复 R15
    popq %r14                         # (%rsp)→%r14: 恢复 R14
    popq %r13                         # (%rsp)→%r13: 恢复 R13
    popq %r12                         # (%rsp)→%r12: 恢复 R12
    popq %r11                         # (%rsp)→%r11: 恢复 R11
    popq %r10                         # (%rsp)→%r10: 恢复 R10
    popq %r9                          # (%rsp)→%r9:  恢复 R9
    popq %r8                          # (%rsp)→%r8:  恢复 R8
    popq %rdi                         # (%rsp)→%rdi: 恢复 RDI
    popq %rsi                         # (%rsp)→%rsi: 恢复 RSI
    popq %rbp                         # (%rsp)→%rbp: 恢复 RBP
    popq %rdx                         # (%rsp)→%rdx: 恢复 RDX
    popq %rcx                         # (%rsp)→%rcx: 恢复 RCX
    popq %rbx                         # (%rsp)→%rbx: 恢复 RBX
    popq %rax                         # (%rsp)→%rax: 恢复 RAX

    /* Step 5: 弹出硬件错误码 */
    addq $8, %rsp                     # $8→%rsp: 跳过 CPU 压入的错误码

    /* Step 6: 中断返回 */
    iretq                             # 中断返回，恢复 RIP/CS/RFLAGS/RSP/SS
.endm

# ============================================================
# ISR Stub 实例化
# ============================================================

# #BP(3) - 断点异常：无错误码
ISR_NOERRCODE isr_bp_stub, 3, handle_bp

# #PF(14) - 页错误异常：有错误码
ISR_ERRCODE isr_pf_stub, 14, handle_pf
