/**
 * @file kernel/arch/x86_64/usermode.S
 * @brief Transition from kernel mode (Ring 0) to user mode (Ring 3)
 *
 * Provides jump_to_usermode() which uses the SYSRET instruction to enter
 * Ring 3.  Before the first call, usermode_init() must have been invoked
 * to configure the STAR MSR with the appropriate CS/SS selectors.
 *
 * SYSRET conventions (Intel SDM Vol. 2A):
 *   - RCX = target RIP (user entry point)
 *   - R11 = saved RFLAGS (restored into RFLAGS by SYSRET)
 *   - RSP must be set to the user stack before SYSRET
 *   - SYSRET loads RCX into RIP, R11 into RFLAGS
 *   - CS is set to STAR[63:48] + 16 (0x33 -> user code, Ring 3)
 *   - SS is set to STAR[63:48] + 8  (0x2B -> user data, Ring 3)
 *
 * AT&T syntax notes:
 *   Operand order: source, destination
 *   Register prefix %, immediate prefix $
 *   Memory addressing: offset(base, index, scale)
 */

.section .text

# ============================================================
# Function: usermode_init
# Purpose: configure STAR MSR for future SYSRET/SYSCALL transitions
# Input:   none
# Output:  none
# Clobbers: %rax, %rdx
# ============================================================
.global usermode_init_asm
.type usermode_init_asm, @function
usermode_init_asm:

    # Write STAR MSR (0xC0000081)
    #   [63:48] = SYSRET base selector (0x23, encodes RPL=3 directly)
    #   [47:32] = SYSCALL CS base selector (0x10, kernel code)
    # SYSRET target CS = STAR[63:48] + 16 = 0x33 (user64 code, RPL=3)
    # SYSRET target SS = STAR[63:48] + 8  = 0x2B (user data, RPL=3)
    # SYSCALL CS = STAR[47:32] = 0x10 (kernel code)
    # SYSCALL SS = STAR[47:32] + 8 = 0x18 (kernel data)
    movq $0x23, %rdx                   # $0x23→%rdx: SYSRET base (RPL=3 embedded)
    shlq $16, %rdx                     # %rdx<<16→%rdx: shift to EDX[31:16] for STAR[63:48]
    orq $0x10, %rdx                    # $0x10|%rdx→%rdx: set EDX[15:0] for STAR[47:32] (kernel CS)
    xorq %rax, %rax                    # 0→%rax: clear low 32 bits (unused)
    movq $0xC0000081, %rcx             # MSR_STAR→%rcx: STAR MSR index
    wrmsr                              # %edx:%eax→MSR[rcx]: write STAR MSR

    # Set SFMASK MSR (0xC0000084) to mask IF on SYSCALL
    movq $0xC0000084, %rcx             # MSR_SFMASK→%rcx: SFMASK MSR index
    xorq %rdx, %rdx                    # 0→%rdx: high 32 bits = 0
    movq $0x200, %rax                  # $0x200→%rax: mask IF bit (bit 9)
    wrmsr                              # %edx:%eax→MSR[rcx]: write SFMASK

    # Enable SYSCALL/SYSRET via EFER.SCE and the No-Execute bit via EFER.NXE
    # (F9 batch 2). NXE is x86_64 baseline: long mode uses PAE page tables,
    # whose bit-63 NX is a reserved-bit #PF until NXE is set, and every
    # x86_64 CPU (and QEMU) supports it -- no CPUID gate needed. Both the BSP
    # and every AP run usermode_init_asm, so this single site enables NX on
    # all cores (sidesteps the BSP/AP MSR-divergence class of bugs).
    movq $0xC0000080, %rcx             # MSR_EFER→%rcx: EFER MSR index
    rdmsr                              # MSR[rcx]→%edx:%eax: read current EFER
    orq $(1 | (1 << 11)), %rax         # SCE(bit0)|NXE(bit11)→%rax: set both
    wrmsr                              # %edx:%eax→MSR[rcx]: write EFER with SCE+NXE

    ret                                 # return to caller

# ============================================================
# Function: jump_to_usermode
# Purpose: transition from Ring 0 to Ring 3 via SYSRET
# Input:   %rdi = entry (user-space RIP)
#          %rsi = user_stack (user-space RSP)
#          %rdx = arg (first argument to user entry, passed via %rdi)
# Output:  does not return (enters user mode)
# Clobbers: all general-purpose registers
# ============================================================
.global jump_to_usermode
.type jump_to_usermode, @function
jump_to_usermode:

    # Set up SYSRET register convention
    # RCX = target RIP for SYSRET
    movq %rdi, %rcx                    # %rdi→%rcx: user entry point (SYSRET loads into RIP)

    # Set user stack pointer
    movq %rsi, %rsp                    # %rsi→%rsp: user stack pointer

    # First argument to user program (passed in %rdi at user entry)
    movq %rdx, %rdi                    # %rdx→%rdi: argument for user entry function

    # R11 = RFLAGS for SYSRET: set IF (bit 9) so interrupts are enabled in user mode
    pushq $0x202                       # $0x202→(%rsp): push RFLAGS with IF|bit1 set
    popq %r11                          # (%rsp)→%r11: pop into R11 (SYSRET restores this)

    # Zero out unused registers to avoid leaking kernel data
    xorq %rax, %rax                    # 0→%rax: clear RAX
    xorq %rbx, %rbx                    # 0→%rbx: clear RBX
    xorq %rdx, %rdx                    # 0→%rdx: clear RDX
    xorq %rsi, %rsi                    # 0→%rsi: clear RSI
    xorq %rbp, %rbp                    # 0→%rbp: clear RBP
    xorq %r8, %r8                      # 0→%r8:  clear R8
    xorq %r9, %r9                      # 0→%r9:  clear R9
    xorq %r10, %r10                    # 0→%r10: clear R10
    xorq %r12, %r12                    # 0→%r12: clear R12
    xorq %r13, %r13                    # 0→%r13: clear R13
    xorq %r14, %r14                    # 0→%r14: clear R14
    xorq %r15, %r15                    # 0→%r15: clear R15

    # swapgs to the user GS base before leaving kernel mode.  In kernel mode
    # MSR_GS_BASE = this CPU's PerCpu block and MSR_KERNEL_GS_BASE = user GS;
    # swapgs flips them so user mode sees MSR_GS_BASE = user GS (0) and the
    # next SYSCALL/IRQ entry's swapgs restores the kernel PerCpu pointer.
    #
    # Hardening (F5-M5 -smp): cli across the swapgs->SYSRETQ window.  After this
    # swapgs we are STILL CPL=0 but GS_BASE is the user value (0); an IRQ taken
    # in the gap is delivered in kernel mode, so the ISR stub's CPL-based entry
    # swapgs does NOT fire, and the handler would run with percpu()==NULL.  This
    # is a latent -smp landmine (a second CPU's timer/IPI landing in a
    # two-instruction window); Linux keeps this whole window at IF=0 for the same
    # reason.  cli blocks maskable IRQs for the two instructions; SYSRETQ then
    # reloads RFLAGS from R11 (IF set) so user mode keeps interrupts.
    cli                                 # block IRQs across swapgs->SYSRETQ
    swapgs                              # kernel_gs↔user_gs: activate user GS

    # Execute SYSRET to enter Ring 3
    # SYSRET: %rcx→RIP, %r11→RFLAGS, CS=STAR[63:48]+16, SS=STAR[63:48]+8
    sysretq                             # enter user mode (Ring 3)

.size jump_to_usermode, . - jump_to_usermode
