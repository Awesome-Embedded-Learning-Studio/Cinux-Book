.section .text

.global JumpToRing3
.type JumpToRing3, @function
JumpToRing3:
    movq %rdi, %rcx
    movq $0x202, %r11
    movq %rsi, %rsp
    sysretq
.size JumpToRing3, . - JumpToRing3
