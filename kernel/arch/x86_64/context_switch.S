.section .text

.global ContextSwitch
.type ContextSwitch, @function
ContextSwitch:
    movq %r15, 0(%rdi)
    movq %r14, 8(%rdi)
    movq %r13, 16(%rdi)
    movq %r12, 24(%rdi)
    movq %rbp, 32(%rdi)
    movq %rbx, 40(%rdi)
    movq %rsp, 48(%rdi)
    leaq .Lresume(%rip), %rax
    movq %rax, 56(%rdi)
    fxsave64 64(%rdi)

    movq 0(%rsi), %r15
    movq 8(%rsi), %r14
    movq 16(%rsi), %r13
    movq 24(%rsi), %r12
    movq 32(%rsi), %rbp
    movq 40(%rsi), %rbx
    movq 48(%rsi), %rsp
    fxrstor64 64(%rsi)
    jmp *56(%rsi)

.Lresume:
    ret
.size ContextSwitch, . - ContextSwitch

.global TaskTrampoline
.type TaskTrampoline, @function
TaskTrampoline:
    sti
    call *%r12
    jmp *%r13
.size TaskTrampoline, . - TaskTrampoline
