.section .text.kentry,"ax",@progbits
.p2align 4

.global KernelEntry
.type KernelEntry, @function
KernelEntry:
    cli
    cld
    movq %cr0, %rax
    andq $~0x4, %rax
    orq $0x2, %rax
    movq %rax, %cr0
    movq %cr4, %rax
    orq $0x600, %rax
    movq %rax, %cr4

    # rdi already carries the BootInfo address from stage2.
    leaq g_kernel_stack_top(%rip), %rsp
    andq $-16, %rsp
    xorl %ebp, %ebp
    call KernelStart

.Lhalt:
    hlt
    jmp .Lhalt
.size KernelEntry, . - KernelEntry

.section .note.GNU-stack,"",@progbits
