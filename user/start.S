.section .text.start,"ax"
.global _start
.type _start, @function
_start:
    xorq %rbp, %rbp
    call UserMain
    movq $60, %rax
    xorq %rdi, %rdi
    syscall
    ud2
.size _start, .-_start
.section .note.GNU-stack,"",@progbits
