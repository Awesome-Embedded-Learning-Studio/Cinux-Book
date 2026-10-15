.section .text

.global SyscallEntry
.type SyscallEntry, @function
SyscallEntry:
    swapgs
    movq %rsp, %gs:8
    movq %gs:0, %rsp
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
    movq %r12, %gs:32
    movq %r13, %gs:40
    movq %r14, %gs:48
    movq %r15, %gs:56
    call EnterFpuGate
    sti
    movq 24(%rsp), %rdi
    movq 32(%rsp), %rsi
    movq 40(%rsp), %rdx
    movq 48(%rsp), %rcx
    call InvokeSyscall
    cli
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
    movq (%rsp), %rsp
    swapgs
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
    movq (%rsp), %rsp
    xorq %rax, %rax
    swapgs
    sysretq
.size ForkChildEntry, . - ForkChildEntry
