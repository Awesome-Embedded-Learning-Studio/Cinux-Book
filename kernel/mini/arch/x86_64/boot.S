/* ==============================================================
 * Cinux Mini Kernel - Bootstrap Entry Point
 * ==============================================================

 * This is the minimal entry point for the mini kernel.
 * Loaded by bootloader at physical address 0x200000 (2MB).
 * Virtual address is 0xFFFFFFFF80000000 (higher-half).
 */

.section .text
.code64

.global _start
.type _start, @function

_start:
    /* Disable interrupts */
    cli

    /* Setup stack - 8KB stack at end of kernel */
    movq $__mini_stack_top, %rsp
    xorq %rbp, %rbp

    /* Clear BSS section */
    movq $__bss_start, %rdi      /* destination */
    movq $__bss_end, %rcx        /* end */
    subq %rdi, %rcx              /* count = end - start */
    xorq %rax, %rax              /* value = 0 */
    rep stosb                    /* clear */

    /* Save BootInfo pointer (passed in %rdi from bootloader) */
    movq %rdi, __boot_info_ptr

    /* Call C++ main */
    movq __boot_info_ptr, %rdi   /* first argument: BootInfo* */
    call mini_kernel_main

    /* Halt if kernel returns */
.halt:
    cli
    hlt
    jmp .halt

.size _start, .-_start

/* ==============================================================
 * Stack Section
 * ============================================================== */
.section .bss
.align 16
.global __mini_stack
.global __mini_stack_top

.set MINI_STACK_SIZE, 0x2000    /* 8KB */

.skip MINI_STACK_SIZE
__mini_stack:
__mini_stack_top:

/* ==============================================================
 * BootInfo Pointer Storage
 * ============================================================== */
.global __boot_info_ptr
.skip 8
__boot_info_ptr:
