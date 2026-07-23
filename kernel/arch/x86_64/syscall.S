/**
 * @file kernel/arch/x86_64/syscall.S
 * @brief SYSCALL entry point for Ring 3 -> Ring 0 transition
 *
 * When a user program executes the SYSCALL instruction:
 *   - RCX <- RIP (return address for SYSRETQ)
 *   - R11 <- RFLAGS
 *   - RIP <- LSTAR MSR value (syscall_entry below)
 *   - CS  <- STAR[47:32] (kernel code segment)
 *   - SS  <- STAR[47:32] + 8 (kernel data segment)
 *   - RFLAGS <- RFLAGS & ~SFMASK
 *
 * Syscall convention (Linux x86_64):
 *   RAX = syscall number
 *   RDI = arg1, RSI = arg2, RDX = arg3
 *   R10 = arg4, R8  = arg5, R9  = arg6
 *   RAX = return value
 *
 * Trap frame layout on kernel stack (pushed in order):
 *   rsp+0:  user RSP
 *   rsp+8:  user RIP (RCX)
 *   rsp+16: user RFLAGS (R11)
 *   rsp+24: syscall number (RAX) / return value after dispatch
 *   rsp+32: user RDI / arg1
 *   rsp+40: user RSI / arg2
 *   rsp+48: user RDX / arg3
 *   rsp+56: user R10 / arg4
 *   rsp+64: user R8  / arg5
 *   rsp+72: user R9  / arg6
 *   rsp+80: user R12
 *   rsp+88: user R13
 *   rsp+96: user R14
 *   rsp+104: user R15
 *   rsp+112: user RBX
 *   rsp+120: user RBP
 *
 * With SIMD enabled in kernel C/C++, syscall_entry also reserves a 512-byte
 * FXSAVE area immediately below the trap frame.  Task::fpu_state is hardcoded
 * at +384 (pinned by process.hpp) for the per-task user-FPU snapshot.
 *
 * AT&T syntax notes:
 *   Operand order: source, destination
 *   Register prefix %, immediate prefix $
 *   Memory addressing: offset(base, index, scale)
 */

.section .text

# ============================================================
# Function: syscall_entry
# Purpose: Ring 3 -> Ring 0 entry via SYSCALL instruction
# Input:   %rax = syscall number, %rdi-%r9 = arguments
#          %rcx = user RIP (saved by SYSCALL)
#          %r11 = user RFLAGS (saved by SYSCALL)
# Output:  %rax = return value (via SYSRETQ)
# User ABI: Linux x86_64 syscall ABI -- %rax is the return value, %rcx/%r11
#           are clobbered by SYSCALL/SYSRET, and all other user registers are
#           preserved.
# ============================================================
.global syscall_entry
.type syscall_entry, @function
syscall_entry:

    # Step 1: SWAPGS -- activate kernel GS base, then STAC (F9 batch 4: SMAP --
    # allow user-memory access in the syscall handler; CLAC clears it before SYSRETQ).
    swapgs                              # kernel_gsbase↔user_gsbase: activate kernel GS
    # stac  (P3: global STAC removed -- SMAP real; user mem only via accessor stac)

    # Step 2: Save user RSP to per-CPU scratch, load kernel stack
    movq %rsp, %gs:8                   # %rsp→%gs:8: save user RSP in scratch slot
    movq %gs:0, %rsp                   # %gs:0→%rsp: load kernel stack pointer

    # Step 3: Build trap frame on kernel stack
    pushq %rbp                         # %rbp→(%rsp): save callee-saved RBP
    pushq %rbx                         # %rbx→(%rsp): save callee-saved RBX
    pushq %r15                         # %r15→(%rsp): save callee-saved R15
    pushq %r14                         # %r14→(%rsp): save callee-saved R14
    pushq %r13                         # %r13→(%rsp): save callee-saved R13
    pushq %r12                         # %r12→(%rsp): save callee-saved R12
    pushq %r9                          # %r9→(%rsp): save arg6
    pushq %r8                          # %r8→(%rsp): save arg5
    pushq %r10                         # %r10→(%rsp): save arg4
    pushq %rdx                         # %rdx→(%rsp): save arg3
    pushq %rsi                         # %rsi→(%rsp): save arg2
    pushq %rdi                         # %rdi→(%rsp): save arg1
    pushq %rax                         # %rax→(%rsp): save syscall number
    pushq %r11                         # %r11→(%rsp): save user RFLAGS
    pushq %rcx                         # %rcx→(%rsp): save user RIP

    # Push user RSP (recovered from %gs:8)
    movq %gs:8, %rax                   # %gs:8→%rax: recover user RSP
    pushq %rax                         # %rax→(%rsp): save user RSP at frame top

    # Frame is now complete:
    #   rsp+0:  user_rsp
    #   rsp+8:  user_rip
    #   rsp+16: user_rflags
    #   rsp+24: syscall_nr
    #   rsp+32: rdi
    #   rsp+40: rsi
    #   rsp+48: rdx
    #   rsp+56: r10
    #   rsp+64: r8
    #   rsp+72: r9
    #   rsp+80: r12
    #   rsp+88: r13
    #   rsp+96: r14
    #   rsp+104: r15
    #   rsp+112: rbx
    #   rsp+120: rbp

    # Preserve user FPU/SSE state before any C/C++ kernel code can use XMM.
    # Keep a stack copy for this syscall's eventual SYSRET and refresh the
    # task snapshot so fork/clone/preemption see the current user SIMD state.
    subq $512, %rsp                    # reserve 512-byte, 16-byte-aligned FXSAVE area
    fxsave (%rsp)                      # save interrupted user FPU/SSE state
    movq %gs:24, %rax                  # current Task* (PerCpu.current)
    testq %rax, %rax
    jz 1f
    fxsave 384(%rax)                   # Task::fpu_state (offset pinned in process.hpp)
1:

    # Step 4: Call syscall_dispatch(nr, a1, a2, a3, a4, a5, a6)
    # System V AMD64: args in %rdi, %rsi, %rdx, %rcx, %r8, %r9, then stack
    # Load arg6 before pushing so offsets are correct
    movq 584(%rsp), %rax               # frame+72→%rax: a6 (original R9)
    subq $8, %rsp                      # align stack to 16 bytes before push
    pushq %rax                         # %rax→(%rsp): push 7th C arg onto stack (shifts frame by +16)

    movq 552(%rsp), %rdi               # frame+24→%rdi: 1st arg = syscall number (+528 offset)
    movq 560(%rsp), %rsi               # frame+32→%rsi: 2nd arg = a1 (original RDI)
    movq 568(%rsp), %rdx               # frame+40→%rdx: 3rd arg = a2 (original RSI)
    movq 576(%rsp), %rcx               # frame+48→%rcx: 4th arg = a3 (original RDX)
    movq 584(%rsp), %r8                # frame+56→%r8:  5th arg = a4 (original R10)
    movq 592(%rsp), %r9                # frame+64→%r9:  6th arg = a5 (original R8)

    call syscall_dispatch              # call C dispatcher

    addq $16, %rsp                     # $16→%rsp: remove 7th arg + alignment pad from stack

    # Step 5: Save return value in GS scratch slot 2 (gs:16)
    #         Cannot use RBX -- it must be restored from the trap frame.
    movq %rax, %gs:16                  # save return value in per-CPU scratch

    # Step 6: Restore user state from trap frame
    movq 512(%rsp), %rax               # frame+0→%rax: recover user RSP
    movq %rax, %gs:8                   # %rax→%gs:8: store user RSP for later restore
    movq 520(%rsp), %rcx               # frame+8→%rcx: user RIP (SYSRETQ loads RIP from RCX)
    movq 528(%rsp), %r11               # frame+16→%r11: user RFLAGS (SYSRETQ loads RFLAGS from R11)
    movq 544(%rsp), %rdi               # frame+32→%rdi: RESTORE user RDI
    movq 552(%rsp), %rsi               # frame+40→%rsi: RESTORE user RSI
    movq 560(%rsp), %rdx               # frame+48→%rdx: RESTORE user RDX
    movq 568(%rsp), %r10               # frame+56→%r10: RESTORE user R10
    movq 576(%rsp), %r8                # frame+64→%r8: RESTORE user R8
    movq 584(%rsp), %r9                # frame+72→%r9: RESTORE user R9
    movq 592(%rsp), %r12               # frame+80→%r12: RESTORE user R12
    movq 600(%rsp), %r13               # frame+88→%r13: RESTORE user R13
    movq 608(%rsp), %r14               # frame+96→%r14: RESTORE user R14
    movq 616(%rsp), %r15               # frame+104→%r15: RESTORE user R15
    movq 624(%rsp), %rbx               # frame+112→%rbx: RESTORE user RBX
    movq 632(%rsp), %rbp               # frame+120→%rbp: RESTORE user RBP
                                      # Musl's inline syscall asm follows the
                                      # Linux ABI: only RAX, RCX, and R11 may
                                      # change.  Preserve the argument regs too,
                                      # not just SysV callee-saved regs.

    # Restore the user FPU/SSE image and deallocate FXSAVE area + trap frame.
    # From here to SYSRETQ, keep the path pure assembly and avoid SIMD use.
    fxrstor (%rsp)                     # restore user FPU/SSE state
    addq $640, %rsp                    # remove 512-byte FXSAVE area + 128-byte frame

    # Step 7: Restore user RSP
    movq %gs:8, %rsp                   # %gs:8→%rsp: switch back to user stack

    # Step 8: Restore return value to RAX
    movq %gs:16, %rax                  # scratch→%rax: restore syscall return value

    # Step 9: SWAPGS back to user GS base.
    # Hardening (F5-M5 -smp): cli across swapgs->SYSRETQ.  After this swapgs we
    # are still CPL=0 but GS_BASE holds the user value (0); an IRQ in the gap is
    # delivered in kernel mode, so the ISR's CPL-based swapgs does not fire and
    # the handler would run with percpu()==NULL.  Same latent -smp window-race as
    # jump_to_usermode (see usermode.S).  SYSRETQ reloads RFLAGS from R11 (user
    # RFLAGS, IF set), so user mode keeps interrupts.
    clac                               # clear AC: re-forbid user access before SYSRETQ (F9 batch 4)
    cli                                # block IRQs across swapgs->SYSRETQ
    swapgs                             # kernel_gsbase↔user_gsbase: activate user GS

    # Step 10: Return to Ring 3
    # SYSRETQ loads: RCX→RIP, R11→RFLAGS, CS=STAR[63:48]+16, SS=STAR[63:48]+8
    sysretq                            # return to Ring 3

.size syscall_entry, . - syscall_entry

# ============================================================
# Function: ret_from_fork
# Purpose: First-chance resume of a user fork()/clone() child
# Input:   %rsp -> a 128-byte pt_regs frame (identical layout to the one
#          syscall_entry builds) copied to the top of the child's CLEAN
#          kernel stack.  Set by prepare_user_fork_context(): ctx.rsp =
#          child->kernel_stack_top - 128, ctx.rip = ret_from_fork.
# Output:  never returns -- SYSRETQs to Ring 3 with RAX=0 (child fork
#          return value), user RIP/RSP/RFLAGS + syscall-preserved regs restored
#          from the frame.
#
# This is the Linux-style fork child path: the child does NOT run on a
# copy of the parent's kernel stack and does not unwind a copied RBP
# chain.  It reads the user register state the parent's syscall captured
# straight out of the frame and returns to user mode exactly like
# syscall_entry's tail.
#
# Frame layout (must match syscall_entry -- see the block comment above):
#   rsp+0:    user_rsp
#   rsp+8:    user_rip
#   rsp+16:   user_rflags
#   rsp+32:   user rdi
#   rsp+40:   user rsi
#   rsp+48:   user rdx
#   rsp+56:   user r10
#   rsp+64:   user r8
#   rsp+72:   user r9
#   rsp+80:   user r12
#   rsp+88:   user r13
#   rsp+96:   user r14
#   rsp+104:  user r15
#   rsp+112:  user rbx
#   rsp+120:  user rbp
#
# GS state on entry: context_switch.S keeps the kernel GS (per-CPU) for
# the whole of kernel mode (it does not swapgs), so like syscall_entry we
# swapgs to the user GS before SYSRETQ.  Interrupts were enabled by
# context_switch's trailing `sti`; we cli across swapgs->SYSRETQ for the
# same SMP window-race reason as syscall_entry (an IRQ in the gap would
# run a CPL=0 handler with the user GS / percpu()==NULL).
# ============================================================
.global ret_from_fork
.type ret_from_fork, @function
ret_from_fork:

    # Restore the syscall-preserved user registers + SYSRETQ control regs from
    # the frame.  %rsp (user_rsp at frame+0) is loaded LAST -- once %rsp
    # leaves the kernel frame we can no longer address it.
    movq 8(%rsp), %rcx                 # frame+8→%rcx: user RIP (SYSRETQ loads RIP from RCX)
    movq 16(%rsp), %r11                # frame+16→%r11: user RFLAGS (SYSRETQ loads RFLAGS from R11)
    movq 32(%rsp), %rdi                # frame+32→%rdi: user RDI
    movq 40(%rsp), %rsi                # frame+40→%rsi: user RSI
    movq 48(%rsp), %rdx                # frame+48→%rdx: user RDX
    movq 56(%rsp), %r10                # frame+56→%r10: user R10
    movq 64(%rsp), %r8                 # frame+64→%r8: user R8
    movq 72(%rsp), %r9                 # frame+72→%r9: user R9
    movq 80(%rsp), %r12                # frame+80→%r12: user R12
    movq 88(%rsp), %r13                # frame+88→%r13: user R13
    movq 96(%rsp), %r14                # frame+96→%r14: user R14
    movq 104(%rsp), %r15               # frame+104→%r15: user R15
    movq 112(%rsp), %rbx               # frame+112→%rbx: user RBX
    movq 120(%rsp), %rbp               # frame+120→%rbp: user RBP
    movq %gs:24, %rax                  # current Task*
    testq %rax, %rax
    jz 1f
    fxrstor 384(%rax)                  # child inherits/restores parent user FPU state
1:
    movq 0(%rsp), %rsp                 # frame+0→%rsp: switch to the user stack

    # Child sees fork()/clone() return 0.
    xorq %rax, %rax                    # 0→%rax: child return value

    # Mirror syscall_entry's SYSRETQ tail exactly (clac re-forbids user
    # access; cli blocks IRQs across the swapgs->SYSRETQ window).
    clac                               # clear AC before leaving kernel mode
    cli                                # block IRQs across swapgs->SYSRETQ
    swapgs                             # kernel_gsbase↔user_gsbase: activate user GS
    sysretq                            # RCX→RIP, R11→RFLAGS -> return to Ring 3

.size ret_from_fork, . - ret_from_fork
