.section .text

.global SyscallEntry
.type SyscallEntry, @function
SyscallEntry:
    movq %rsp, %gs:8
    movq %gs:0, %rsp
    pushq %r15
    pushq %r14
    pushq %r13
    pushq %r12
    pushq %rbp
    pushq %rbx
    pushq %r9
    pushq %r8
    pushq %r10
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rax
    pushq %r11
    pushq %rcx
    pushq %gs:8
    movq %rsp, %gs:24
    call EnterFpuGate
    sti
    movq %rsp, %rdi
    call InvokeSyscall
    cli
    movq %rax, 24(%rsp)
    pushq %rax
    subq $8, %rsp
    call ExitFpuGate
    addq $8, %rsp
    popq %rax
    movq 32(%rsp), %rdi
    movq 40(%rsp), %rsi
    movq 48(%rsp), %rdx
    movq 56(%rsp), %r10
    movq 64(%rsp), %r8
    movq 72(%rsp), %r9
    movq 8(%rsp), %rcx
    movq 16(%rsp), %r11
    movq 80(%rsp), %rbx
    movq 88(%rsp), %rbp
    movq 96(%rsp), %r12
    movq 104(%rsp), %r13
    movq 112(%rsp), %r14
    movq 120(%rsp), %r15
    movq (%rsp), %rsp
    sysretq
.size SyscallEntry, . - SyscallEntry

.global ForkChildEntry
.type ForkChildEntry, @function
ForkChildEntry:
    movq 32(%rsp), %rdi
    movq 40(%rsp), %rsi
    movq 48(%rsp), %rdx
    movq 56(%rsp), %r10
    movq 64(%rsp), %r8
    movq 72(%rsp), %r9
    movq 8(%rsp), %rcx
    movq 16(%rsp), %r11
    movq 80(%rsp), %rbx
    movq 88(%rsp), %rbp
    movq 96(%rsp), %r12
    movq 104(%rsp), %r13
    movq 112(%rsp), %r14
    movq 120(%rsp), %r15
    movq (%rsp), %rsp
    xorq %rax, %rax
    sysretq
.size ForkChildEntry, . - ForkChildEntry
