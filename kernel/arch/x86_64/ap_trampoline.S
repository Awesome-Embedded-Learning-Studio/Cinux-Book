.section .text.ap_trampoline,"ax",@progbits
.balign 16
.global ApTrampolineStart
.global ApTrampolineEnd
.global ApBootRoot
.global ApKernelRoot
.global ApStack
.global ApCpu
.global ApEntry
.global ApEfer
.global ApCr4
.code16
ApTrampolineStart:
    cli
    cld
    xorw %ax, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    lgdtl (ap_gdt_pointer - ApTrampolineStart + 0x8000)
    movl %cr0, %eax
    orl $1, %eax
    movl %eax, %cr0
    ljmpl $0x08, $(ap_protected - ApTrampolineStart + 0x8000)
.code32
ap_protected:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movl (ApCr4 - ApTrampolineStart + 0x8000), %eax
    movl %eax, %cr4
    movl (ApBootRoot - ApTrampolineStart + 0x8000), %eax
    movl %eax, %cr3
    movl $0xC0000080, %ecx
    movl (ApEfer - ApTrampolineStart + 0x8000), %eax
    xorl %edx, %edx
    wrmsr
    movl %cr0, %eax
    andl $~0xC, %eax
    orl $0x80010003, %eax
    movl %eax, %cr0
    ljmpl $0x18, $(ap_long - ApTrampolineStart + 0x8000)
.code64
ap_long:
    movl ApCpu(%rip), %edi
    movq ApKernelRoot(%rip), %rsi
    movq ApStack(%rip), %rdx
    movq ApEntry(%rip), %rax
    jmp *%rax
.balign 8
ap_gdt:
    .quad 0
    .quad 0x00CF9A000000FFFF
    .quad 0x00CF92000000FFFF
    .quad 0x00AF9A000000FFFF
ap_gdt_end:
ap_gdt_pointer:
    .word ap_gdt_end - ap_gdt - 1
    .long ap_gdt - ApTrampolineStart + 0x8000
.balign 8
ApBootRoot:
    .long 0
ApCpu:
    .long 0
ApEfer:
    .long 0
ApCr4:
    .long 0
ApKernelRoot:
    .quad 0
ApStack:
    .quad 0
ApEntry:
    .quad 0
ApTrampolineEnd:

.section .text,"ax",@progbits
.global ApEntryLong
.type ApEntryLong,@function
ApEntryLong:
    movq %rsi, %cr3
    movq %rdx, %rsp
    xorq %rbp, %rbp
    call ApMain
1:
    cli
    hlt
    jmp 1b
.size ApEntryLong, . - ApEntryLong
.section .note.GNU-stack,"",@progbits
