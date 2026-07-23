/**
 * @file kernel/arch/x86_64/interrupts.S
 * @brief ISR (Interrupt Service Routine) stubs for the big kernel
 *
 * Provides ISR stubs for all CPU exceptions configured in idt.cpp.
 *
 * Workflow for each stub:
 *   1. For exceptions without a hardware error code: push dummy 0
 *   2. Save all general-purpose registers (construct InterruptFrame)
 *   3. Pass %rsp as first argument to the C handler
 *   4. Call the C handler function
 *   5. Restore all general-purpose registers
 *   6. Remove error code from stack
 *   7. Return via IRETQ
 *
 * AT&T syntax notes:
 *   Operand order: source, destination
 *   Register prefix %, immediate prefix $
 *   Memory addressing: offset(base, index, scale)
 */

.section .text

# ============================================================
# ISR Stub macro: exception without hardware error code
# ============================================================
# Push a dummy error code 0 to keep the InterruptFrame layout
# uniform across all handlers.
# ============================================================

.macro ISR_NOERRCODE name handler
.global \name
.type \name, @function
\name:
    /* Push dummy error code 0 */
    pushq $0                          # $0→(%rsp): push dummy error code 0

    /* Save all general-purpose registers */
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

    /*
     * Stack alignment padding.
     *
     * The CPU pushes 5 qwords (SS, RSP, RFLAGS, CS, RIP = 40 bytes)
     * before entering the ISR.  We push 17 qwords above (1 dummy error
     * code + 15 GPRs + 1 padding = 136 bytes).  Total: 176 bytes,
     * which is a multiple of 16.  The subsequent 'call' pushes 8 bytes,
     * giving the C handler the standard ABI entry alignment of RSP % 16 == 8.
     */
    pushq $0                          # alignment padding (8 bytes)

    /* F4-M3 P1-2: conditional swapgs on entry.  If entered from user mode
       (CPL=3 in the CPU-saved CS), swapgs to activate this CPU's kernel GS
       base (PerCpu block) so the C handler may call percpu().  Saved CS sits
       at RSP+144 (padding + 15 GPRs + error code + RIP).  %rax is still saved
       on the stack, so clobbering it as scratch here is safe (restored at exit). */
    movq 144(%rsp), %rax              # saved CS→%rax
    testb $3, %al                     # CPL bits (low 2 bits of CS)
    jz 1f                             # kernel mode (CPL=0) -> skip swapgs
    swapgs                            # user mode -> activate kernel GS
    # stac  (P3: global STAC removed -- SMAP real; user mem only via accessor stac)
1:

    /* Preserve interrupted FPU/SSE state across kernel C/C++ handlers.  If the
       interrupt came from user mode, also refresh Task::fpu_state so a timer
       preemption switch has the current user SIMD image. */
    subq $512, %rsp                   # 512-byte, 16-byte-aligned FXSAVE area
    fxsave (%rsp)
    movq 656(%rsp), %rax              # saved CS (old RSP+144)
    testb $3, %al
    jz 2f
    movq %gs:24, %rax                 # current Task* (PerCpu.current)
    testq %rax, %rax
    jz 2f
    fxsave 384(%rax)                  # Task::fpu_state (offset pinned in process.hpp)
2:

    /* Pass InterruptFrame* as first argument (skip alignment padding) */
    leaq 520(%rsp), %rdi              # FXSAVE + padding -> InterruptFrame

    /* Call the C handler function */
    call \handler                     # call the corresponding C exception handler

    /* F3-M1: deliver one pending signal before returning to user mode. */
    leaq 520(%rsp), %rdi              # FXSAVE + padding -> InterruptFrame
    call signal_check_deliver_isr     # may rewrite the frame to enter a handler

    /* Restore interrupted FPU/SSE state before unwinding the frame. */
    fxrstor (%rsp)
    addq $512, %rsp

    /* Remove alignment padding */
    addq $8, %rsp                     # $8→%rsp: skip alignment padding

    /* F4-M3 P1-2: conditional swapgs on exit.  If resuming user mode, swapgs
       back so user mode sees MSR_GS_BASE = user GS.  Padding is now removed,
       so saved CS sits at RSP+136.  %rax is clobbered as scratch but restored
       by the pop below (its saved value is still on the stack at its slot). */
    movq 136(%rsp), %rax              # saved CS→%rax
    testb $3, %al                     # CPL bits
    jz 1f                             # kernel mode -> skip swapgs
    clac                              # F9 batch 4: SMAP -- re-forbid user access before IRETQ
    swapgs                            # user mode -> restore user GS
1:

    /* Restore all general-purpose registers */
    popq %r15                         # (%rsp)→%r15: restore R15
    popq %r14                         # (%rsp)→%r14: restore R14
    popq %r13                         # (%rsp)→%r13: restore R13
    popq %r12                         # (%rsp)→%r12: restore R12
    popq %r11                         # (%rsp)→%r11: restore R11
    popq %r10                         # (%rsp)→%r10: restore R10
    popq %r9                          # (%rsp)→%r9: restore R9
    popq %r8                          # (%rsp)→%r8: restore R8
    popq %rdi                         # (%rsp)→%rdi: restore RDI
    popq %rsi                         # (%rsp)→%rsi: restore RSI
    popq %rbp                         # (%rsp)→%rbp: restore RBP
    popq %rdx                         # (%rsp)→%rdx: restore RDX
    popq %rcx                         # (%rsp)→%rcx: restore RCX
    popq %rbx                         # (%rsp)→%rbx: restore RBX
    popq %rax                         # (%rsp)→%rax: restore RAX

    /* Remove error code from stack */
    addq $8, %rsp                     # $8→%rsp: skip error code

    /* Interrupt return */
    iretq                             # interrupt return, restore RIP/CS/RFLAGS/RSP/SS
.endm

# ============================================================
# ISR Stub macro: exception with hardware error code
# ============================================================
# The CPU has already pushed the error code, so no dummy push needed.
# ============================================================

.macro ISR_ERRCODE name handler
.global \name
.type \name, @function
\name:
    /* Save all general-purpose registers (error code already on stack) */
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

    /*
     * Stack alignment padding.
     *
     * For error-code exceptions the CPU pushes 6 qwords
     * (SS, RSP, RFLAGS, CS, RIP, error = 48 bytes) before entering
     * the ISR.  We push 15 GPRs + 1 padding = 16 qwords (128 bytes).
     * Total: 176 bytes, a multiple of 16.  The 'call' then gives the
     * C handler the standard ABI entry alignment of RSP % 16 == 8.
     */
    pushq $0                          # alignment padding (8 bytes)

    /* F4-M3 P1-2: conditional swapgs on entry.  If entered from user mode
       (CPL=3 in the CPU-saved CS), swapgs to activate this CPU's kernel GS
       base (PerCpu block) so the C handler may call percpu().  Saved CS sits
       at RSP+144 (padding + 15 GPRs + error code + RIP).  %rax is still saved
       on the stack, so clobbering it as scratch here is safe (restored at exit). */
    movq 144(%rsp), %rax              # saved CS→%rax
    testb $3, %al                     # CPL bits (low 2 bits of CS)
    jz 1f                             # kernel mode (CPL=0) -> skip swapgs
    swapgs                            # user mode -> activate kernel GS
    # stac  (P3: global STAC removed -- SMAP real; user mem only via accessor stac)
1:

    /* Preserve interrupted FPU/SSE state across kernel C/C++ handlers.  If the
       interrupt came from user mode, also refresh Task::fpu_state so a timer
       preemption switch has the current user SIMD image. */
    subq $512, %rsp                   # 512-byte, 16-byte-aligned FXSAVE area
    fxsave (%rsp)
    movq 656(%rsp), %rax              # saved CS (old RSP+144)
    testb $3, %al
    jz 2f
    movq %gs:24, %rax                 # current Task* (PerCpu.current)
    testq %rax, %rax
    jz 2f
    fxsave 384(%rax)                  # Task::fpu_state (offset pinned in process.hpp)
2:

    /* Pass InterruptFrame* as first argument (skip alignment padding) */
    leaq 520(%rsp), %rdi              # FXSAVE + padding -> InterruptFrame

    /* Call the C handler function */
    call \handler                     # call the corresponding C exception handler

    /* F3-M1: deliver one pending signal before returning to user mode. */
    leaq 520(%rsp), %rdi              # FXSAVE + padding -> InterruptFrame
    call signal_check_deliver_isr     # may rewrite the frame to enter a handler

    /* Restore interrupted FPU/SSE state before unwinding the frame. */
    fxrstor (%rsp)
    addq $512, %rsp

    /* Remove alignment padding */
    addq $8, %rsp                     # $8→%rsp: skip alignment padding

    /* F4-M3 P1-2: conditional swapgs on exit.  If resuming user mode, swapgs
       back so user mode sees MSR_GS_BASE = user GS.  Padding is now removed,
       so saved CS sits at RSP+136.  %rax is clobbered as scratch but restored
       by the pop below (its saved value is still on the stack at its slot). */
    movq 136(%rsp), %rax              # saved CS→%rax
    testb $3, %al                     # CPL bits
    jz 1f                             # kernel mode -> skip swapgs
    clac                              # F9 batch 4: SMAP -- re-forbid user access before IRETQ
    swapgs                            # user mode -> restore user GS
1:

    /* Restore all general-purpose registers */
    popq %r15                         # (%rsp)→%r15: restore R15
    popq %r14                         # (%rsp)→%r14: restore R14
    popq %r13                         # (%rsp)→%r13: restore R13
    popq %r12                         # (%rsp)→%r12: restore R12
    popq %r11                         # (%rsp)→%r11: restore R11
    popq %r10                         # (%rsp)→%r10: restore R10
    popq %r9                          # (%rsp)→%r9: restore R9
    popq %r8                          # (%rsp)→%r8: restore R8
    popq %rdi                         # (%rsp)→%rdi: restore RDI
    popq %rsi                         # (%rsp)→%rsi: restore RSI
    popq %rbp                         # (%rsp)→%rbp: restore RBP
    popq %rdx                         # (%rsp)→%rdx: restore RDX
    popq %rcx                         # (%rsp)→%rcx: restore RCX
    popq %rbx                         # (%rsp)→%rbx: restore RBX
    popq %rax                         # (%rsp)→%rax: restore RAX

    /* Remove hardware error code from stack */
    addq $8, %rsp                     # $8→%rsp: skip CPU-pushed error code

    /* Interrupt return */
    iretq                             # interrupt return, restore RIP/CS/RFLAGS/RSP/SS
.endm

# ============================================================
# ISR Stub macro: hardware IRQ
# ============================================================
# IRQs push no hardware error code, so the frame mirrors ISR_NOERRCODE exactly.
# The difference: the C handler does NOT EOI -- THIS stub sends irq_eoi_isr()
# after the handler returns, then runs irq_exit() (deferred preemption) before
# signal delivery.  EOI lives in one place, so an IRQ handler cannot forget it
# or call the wrong back-end (the keyboard PIC::send_eoi bug: in APIC mode the
# PIC EOI is a no-op, so vector 0x21 stayed in the LAPIC ISR, raised the
# processor priority, and blocked the PIT + every lower/equal-priority IRQ).
#
# Prologue/epilogue are a copy of ISR_NOERRCODE's; if the swapgs discipline
# there changes, keep this in sync (a future cleanup could factor the shared
# frame push/pop into a helper macro).
# ============================================================

.macro ISR_IRQ name handler irq
.global \name
.type \name, @function
\name:
    /* Push dummy error code 0 */
    pushq $0                          # $0->(%rsp): push dummy error code 0

    /* Save all general-purpose registers */
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rbp
    pushq %rsi
    pushq %rdi
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    pushq $0                          # alignment padding (8 bytes)

    /* conditional swapgs on entry (CPL=3 -> activate kernel GS) */
    movq 144(%rsp), %rax              # saved CS->%rax
    testb $3, %al                     # CPL bits
    jz 1f                             # kernel mode -> skip swapgs
    swapgs                            # user mode -> activate kernel GS
    # stac  (P3: global STAC removed -- SMAP real; user mem only via accessor stac)
1:

    /* Preserve interrupted FPU/SSE state across kernel C/C++ handlers.  If the
       interrupt came from user mode, also refresh Task::fpu_state so a timer
       preemption switch has the current user SIMD image. */
    subq $512, %rsp                   # 512-byte, 16-byte-aligned FXSAVE area
    fxsave (%rsp)
    movq 656(%rsp), %rax              # saved CS (old RSP+144)
    testb $3, %al
    jz 2f
    movq %gs:24, %rax                 # current Task* (PerCpu.current)
    testq %rax, %rax
    jz 2f
    fxsave 384(%rax)                  # Task::fpu_state (offset pinned in process.hpp)
2:

    /* Pass InterruptFrame* as first argument (skip alignment padding) */
    leaq 520(%rsp), %rdi              # FXSAVE + padding -> InterruptFrame

    /* Call the C handler.  It must NOT EOI -- this stub owns the EOI below. */
    call \handler

    /* EOI the active back-end (PIC or LAPIC).  \irq is the ISA IRQ number
       (used by PIC EOI; ignored in APIC mode, where lapic.eoi takes no arg).
       MSI-X (0x40) and the reschedule IPI (0xE0) are APIC-only, so their
       \irq is unused.  (The PIT is NOT an ISR_IRQ stub -- it keeps an early
       EOI inside its handler so the timer can re-fire across the inline
       preemption switch; see pit.cpp.  Device IRQs do not preempt, so no
       deferred-switch is needed here.) */
    mov $\irq, %edi                   # ISA IRQ for PIC EOI
    call irq_eoi_isr                  # EOI via the active back-end

    /* deliver one pending signal before returning to user mode */
    leaq 520(%rsp), %rdi              # FXSAVE + padding -> InterruptFrame
    call signal_check_deliver_isr     # may rewrite the frame to enter a handler

    /* Restore interrupted FPU/SSE state before unwinding the frame. */
    fxrstor (%rsp)
    addq $512, %rsp

    /* Remove alignment padding */
    addq $8, %rsp                     # $8->%rsp: skip alignment padding

    /* conditional swapgs on exit (CPL=3 -> restore user GS) */
    movq 136(%rsp), %rax              # saved CS->%rax
    testb $3, %al                     # CPL bits
    jz 1f                             # kernel mode -> skip swapgs
    clac                              # F9 batch 4: SMAP -- re-forbid user access before IRETQ
    swapgs                            # user mode -> restore user GS
1:

    /* Restore all general-purpose registers */
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rdi
    popq %rsi
    popq %rbp
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax

    /* Remove error code from stack */
    addq $8, %rsp                     # $8->%rsp: skip error code

    /* Interrupt return */
    iretq
.endm

# ============================================================
# ISR Stub instantiation
# ============================================================

# Exceptions without error code
ISR_NOERRCODE isr_de_stub,  handle_de       /* #DE(0):  Divide Error */
ISR_NOERRCODE isr_db_stub,  handle_db       /* #DB(1):  Debug */
ISR_NOERRCODE isr_nmi_stub, handle_nmi      /* NMI(2) */
ISR_NOERRCODE isr_bp_stub,  handle_bp       /* #BP(3):  Breakpoint */
ISR_NOERRCODE isr_of_stub,  handle_of       /* #OF(4):  Overflow */
ISR_NOERRCODE isr_br_stub,  handle_br       /* #BR(5):  BOUND Range */
ISR_NOERRCODE isr_ud_stub,  handle_ud       /* #UD(6):  Invalid Opcode */
ISR_NOERRCODE isr_nm_stub,  handle_nm       /* #NM(7):  Device Not Available */

# Exceptions with error code
ISR_ERRCODE   isr_df_stub,  handle_df       /* #DF(8):  Double Fault */
ISR_ERRCODE   isr_ts_stub,  handle_ts       /* #TS(10): Invalid TSS */
ISR_ERRCODE   isr_np_stub,  handle_np       /* #NP(11): Segment Not Present */
ISR_ERRCODE   isr_ss_stub,  handle_ss       /* #SS(12): Stack Fault */
ISR_ERRCODE   isr_gp_stub,  handle_gp       /* #GP(13): General Protection */
ISR_ERRCODE   isr_pf_stub,  handle_pf       /* #PF(14): Page Fault */

/* F3-M1: sigreturn trampoline gate (vector 0x80).  Entered by the int $0x80
 * instruction that signal_setup_frame writes onto the user stack; restores
 * the saved user context.  DPL=3 (user-callable), configured in idt.cpp. */
ISR_NOERRCODE isr_sigreturn_stub, sigreturn_handler

# ============================================================
# Hardware IRQ stubs (PIC remapped vectors 0x20-0x2F)
# ============================================================
# Master PIC: IRQ0-7 → INT 0x20-0x27
# Slave PIC:  IRQ8-15 → INT 0x28-0x2F
#
# For milestone 014, IRQ0 (PIT) and IRQ1 (Keyboard) have real handlers.
# All other IRQs get a default no-op handler for safety.
# Note: The ISR_IRQ stub owns the EOI (irq_eoi_isr) -- C handlers do NOT EOI.
# ============================================================

/* Master PIC IRQs -- the 3rd arg is the ISA IRQ number (for PIC EOI; ignored
 * in APIC mode).  The ISR_IRQ stub owns the EOI; the C handlers do not EOI.
 * EXCEPTION: IRQ0 (PIT) uses ISR_NOERRCODE and EOIs early inside its handler
 * (before Scheduler::tick's inline preemption switch) so the timer is free to
 * re-fire and preempt the task we switch to.  Device IRQs do not preempt, so
 * they use ISR_IRQ (stub-owned EOI) and need no early-EOI trick. */
ISR_NOERRCODE irq0_stub,  pit_irq0_handler      /* IRQ0(0x20): PIT (early EOI in handler) */
ISR_IRQ irq1_stub,  keyboard_irq1_handler, 1    /* IRQ1(0x21): Keyboard */
ISR_IRQ irq2_stub,  irq_default_handler, 2   /* IRQ2(0x22): Cascade */
ISR_IRQ irq3_stub,  irq_default_handler, 3   /* IRQ3(0x23): COM2 */
ISR_IRQ irq4_stub,  irq_default_handler, 4   /* IRQ4(0x24): COM1 */
ISR_IRQ irq5_stub,  irq_default_handler, 5   /* IRQ5(0x25): LPT2 */
ISR_IRQ irq6_stub,  irq_default_handler, 6   /* IRQ6(0x26): Floppy */
ISR_IRQ irq7_stub,  irq_default_handler, 7   /* IRQ7(0x27): LPT1 */

/* Slave PIC IRQs */
ISR_IRQ irq8_stub,  irq_default_handler, 8   /* IRQ8(0x28):  RTC */
ISR_IRQ irq9_stub,  irq_default_handler, 9   /* IRQ9(0x29):  Free */
ISR_IRQ irq10_stub, irq_default_handler, 10  /* IRQ10(0x2A): Free */
ISR_IRQ irq11_stub, irq_default_handler, 11  /* IRQ11(0x2B): Free */
ISR_IRQ irq12_stub, mouse_irq12_handler, 12  /* IRQ12(0x2C): PS/2 Mouse */
ISR_IRQ irq13_stub, irq_default_handler, 13  /* IRQ13(0x2D): FPU */
ISR_IRQ irq14_stub, irq_default_handler, 14  /* IRQ14(0x2E): Primary ATA */
ISR_IRQ irq15_stub, irq_default_handler, 15  /* IRQ15(0x2F): Secondary ATA */

# ============================================================
# Reschedule IPI (F4-M4 M4-2)
# ============================================================
# Vector 0xE0, sent by the BSP (wake_idle_ap) to pull an idle AP out of hlt
# so it re-checks the shared run queue.  The handler body is now empty (the
# ISR_IRQ stub EOIs); the AP's idle loop does the actual reschedule after the
# stub returns.  \irq=0 is unused (IPIs are APIC-only).
# ============================================================
ISR_IRQ reschedule_ipi_stub, reschedule_ipi_handler, 0

# ============================================================
# xHCI event-ring MSI-X interrupt (F5-M5 Batch 0C)
# ============================================================
# Vector 0x40 (kXhciIrqVector), registered in irq_init.  MSI-X is not
# programmed until Batch 2C, so this never fires prematurely; the handler
# (usb/xhci_irq.cpp, or a default stub when CINUX_USB is off) counts the
# interrupt and runs the controller's event-ring hook.  The ISR_IRQ stub
# EOIs.  \irq=0 is unused (MSI-X is APIC-only).
# ============================================================
ISR_IRQ xhci_irq_stub, xhci_irq_handler, 0

# ============================================================
# LAPIC timer (F5-M5 -smp per-CPU preemption)
# ============================================================
# Vector 0x30 (kLapicTimerVector), fired by each AP's local APIC timer
# (programmed in ap_main).  Drives Scheduler::tick() on APs -- the BSP is
# preempted by the PIT, which the IOAPIC redirects to the BSP only, so APs
# need their own timer or a spin-waiting task (e.g. shell on stdin) deadlocks
# the AP.  Uses ISR_NOERRCODE with an EARLY EOI inside the handler, exactly
# like irq0_stub: the timer must be re-armed (EOI'd) before the inline
# preemption switch in Scheduler::tick(), or the next tick could be missed.
# ============================================================
ISR_NOERRCODE lapic_timer_stub, lapic_timer_handler

# ============================================================
# Network poll timer (F5-M6 e1000): no-op LAPIC timer ISR
# ============================================================
# Vector 0x30 (kLapicTimerVector), armed by the TEST kernel (main_test.cpp)
# so the e1000 RX poll path can sti+hlt between polls: hlt lets QEMU's
# device-model main loop run and pull SLIRP replies into the e1000 ring, and
# this IRQ wakes the CPU.  The handler only EOIs (direct LAPIC EOI) -- no
# Scheduler::tick(), so it cannot perturb the test suite.  Dormant in
# production, where the PIT/LAPIC tick already keeps the main loop alive.
# ============================================================
ISR_NOERRCODE net_timer_stub, net_timer_handler
