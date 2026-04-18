/**
 * @file kernel/mini/arch/x86_64/interrupts.S
 * @brief ISR (Interrupt Service Routine) stub assembly implementation
 *
 * Provides ISR stubs for #BP(3) and #PF(14) exceptions.
 *
 * Workflow for each stub:
 *   1. Save all general-purpose registers onto the stack (construct InterruptFrame)
 *   2. For exceptions without a hardware error code (#BP), push a dummy error code 0 to keep the stack aligned
 *   3. For exceptions with a hardware error code (#PF), the CPU has already pushed it, no extra handling needed
 *   4. Pass the current stack pointer (pointing to InterruptFrame) as the first argument to the C handler
 *   5. Call the C handler function (handle_bp / handle_pf)
 *   6. After the function returns, restore all general-purpose registers
 *   7. Return to the interrupted code via IRETQ
 *
 * AT&T syntax notes:
 *   Operand order: source operand, destination operand
 *   Register prefix %, immediate prefix $
 *   Memory addressing: offset(base, index, scale)
 */

.section .text

# ============================================================
# ISR Stub macro: exception without hardware error code
# ============================================================
# For exceptions like #BP that do not produce an error code, we must
# manually push a dummy error code 0 to keep the stack frame layout
# uniform, so the C handler can correctly parse the InterruptFrame
# ============================================================

.macro ISR_NOERRCODE name vector handler
.global \name
.type \name, @function
\name:
    /* Step 1: Push dummy error code 0 to keep the stack aligned */
    pushq $0                          # $0→(%rsp): push dummy error code 0

    /* Step 2: Save all general-purpose registers */
    pushq %rax                        # %rax→(%rsp): save RAX
    pushq %rbx                        # %rbx→(%rsp): save RBX
    pushq %rcx                        # %rcx→(%rsp): save RCX
    pushq %rdx                        # %rdx→(%rsp): save RDX
    pushq %rbp                        # %rbp→(%rsp): save RBP
    pushq %rsi                        # %rsi→(%rsp): save RSI
    pushq %rdi                        # %rdi→(%rsp): save RDI
    pushq %r8                         # %r8→(%rsp): save R8
    pushq %r9                         # %r9→(%rsp): save R9
    pushq %r10                        # %r10→(%rsp): save R10
    pushq %r11                        # %r11→(%rsp): save R11
    pushq %r12                        # %r12→(%rsp): save R12
    pushq %r13                        # %r13→(%rsp): save R13
    pushq %r14                        # %r14→(%rsp): save R14
    pushq %r15                        # %r15→(%rsp): save R15

    /* Step 3: Pass InterruptFrame* as the first argument */
    movq %rsp, %rdi                   # %rsp→%rdi: pass stack pointer as first argument

    /* Step 4: Call the C handler function */
    call \handler                     # call the corresponding C exception handler

    /* Step 5: Restore all general-purpose registers */
    popq %r15                         # (%rsp)→%r15: restore R15
    popq %r14                         # (%rsp)→%r14: restore R14
    popq %r13                         # (%rsp)→%r13: restore R13
    popq %r12                         # (%rsp)→%r12: restore R12
    popq %r11                         # (%rsp)→%r11: restore R11
    popq %r10                         # (%rsp)→%r10: restore R10
    popq %r9                          # (%rsp)→%r9:  restore R9
    popq %r8                          # (%rsp)→%r8:  restore R8
    popq %rdi                         # (%rsp)→%rdi: restore RDI
    popq %rsi                         # (%rsp)→%rsi: restore RSI
    popq %rbp                         # (%rsp)→%rbp: restore RBP
    popq %rdx                         # (%rsp)→%rdx: restore RDX
    popq %rcx                         # (%rsp)→%rcx: restore RCX
    popq %rbx                         # (%rsp)→%rbx: restore RBX
    popq %rax                         # (%rsp)→%rax: restore RAX

    /* Step 6: Pop the dummy error code */
    addq $8, %rsp                     # $8→%rsp: skip dummy error code

    /* Step 7: Interrupt return */
    iretq                             # interrupt return, restore RIP/CS/RFLAGS/RSP/SS
.endm

# ============================================================
# ISR Stub macro: exception with hardware error code
# ============================================================
# For exceptions like #PF that produce an error code, the CPU has
# already automatically pushed the error code before entering the ISR,
# so no manual dummy error code push is needed
# ============================================================

.macro ISR_ERRCODE name vector handler
.global \name
.type \name, @function
\name:
    /* Step 1: Save all general-purpose registers (error code already on stack) */

    pushq %rax                        # %rax→(%rsp): save RAX
    pushq %rbx                        # %rbx→(%rsp): save RBX
    pushq %rcx                        # %rcx→(%rsp): save RCX
    pushq %rdx                        # %rdx→(%rsp): save RDX
    pushq %rbp                        # %rbp→(%rsp): save RBP
    pushq %rsi                        # %rsi→(%rsp): save RSI
    pushq %rdi                        # %rdi→(%rsp): save RDI
    pushq %r8                         # %r8→(%rsp): save R8
    pushq %r9                         # %r9→(%rsp): save R9
    pushq %r10                        # %r10→(%rsp): save R10
    pushq %r11                        # %r11→(%rsp): save R11
    pushq %r12                        # %r12→(%rsp): save R12
    pushq %r13                        # %r13→(%rsp): save R13
    pushq %r14                        # %r14→(%rsp): save R14
    pushq %r15                        # %r15→(%rsp): save R15

    /* Step 2: Pass InterruptFrame* as the first argument */
    movq %rsp, %rdi                   # %rsp→%rdi: pass stack pointer as first argument

    /* Step 3: Call the C handler function */
    call \handler                     # call the corresponding C exception handler

    /* Step 4: Restore all general-purpose registers */
    popq %r15                         # (%rsp)→%r15: restore R15
    popq %r14                         # (%rsp)→%r14: restore R14
    popq %r13                         # (%rsp)→%r13: restore R13
    popq %r12                         # (%rsp)→%r12: restore R12
    popq %r11                         # (%rsp)→%r11: restore R11
    popq %r10                         # (%rsp)→%r10: restore R10
    popq %r9                          # (%rsp)→%r9:  restore R9
    popq %r8                          # (%rsp)→%r8:  restore R8
    popq %rdi                         # (%rsp)→%rdi: restore RDI
    popq %rsi                         # (%rsp)→%rsi: restore RSI
    popq %rbp                         # (%rsp)→%rbp: restore RBP
    popq %rdx                         # (%rsp)→%rdx: restore RDX
    popq %rcx                         # (%rsp)→%rcx: restore RCX
    popq %rbx                         # (%rsp)→%rbx: restore RBX
    popq %rax                         # (%rsp)→%rax: restore RAX

    /* Step 5: Pop the hardware error code */
    addq $8, %rsp                     # $8→%rsp: skip CPU-pushed error code

    /* Step 6: Interrupt return */
    iretq                             # interrupt return, restore RIP/CS/RFLAGS/RSP/SS
.endm

# ============================================================
# ISR Stub instantiation
# ============================================================

# #BP(3) - Breakpoint exception: no error code
ISR_NOERRCODE isr_bp_stub, 3, handle_bp

# #PF(14) - Page fault exception: has error code
ISR_ERRCODE isr_pf_stub, 14, handle_pf
