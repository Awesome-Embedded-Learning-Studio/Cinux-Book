ENTRY(KernelEntry)
SECTIONS
{
    . = 0xFFFFFFFF80200000;
    __vma_start = .;
    .image_header : AT(0x200000) {
        KEEP(*(.image_header))
        . = ALIGN(16);
    }
    .text : ALIGN(16) {
        *(.text.kentry)
        *(.text.exc)
        *(.text*)
    }
    .rodata : ALIGN(16) { KEEP(*(.rodata.font)) *(.rodata*) }
    .user ALIGN(4096) : { KEEP(*(usertext.user)) }
    .data : ALIGN(16) {
        *(.data*)
        . = ALIGN(16);
        g_test_start = .;
        KEEP(*(.test_cases))
        g_test_end = .;
    }
    .bss : ALIGN(16) {
        g_kernel_bss_start = .;
        *(.bss*)
        *(COMMON)
        g_kernel_bss_end = .;
    }
    .stack (NOLOAD) : ALIGN(16) {
        . = . + 16384;
        g_kernel_stack_top = .;
    }
    __vma_end = .;
    g_kernel_load_paddr = LOADADDR(.image_header);
    __file_end_lma = SIZEOF(.data) ? LOADADDR(.data) + SIZEOF(.data) :
                    (SIZEOF(.rodata) ? LOADADDR(.rodata) + SIZEOF(.rodata) :
                     LOADADDR(.text) + SIZEOF(.text));
    __mem_end_lma = g_kernel_load_paddr + (__vma_end - __vma_start);
    g_kernel_entry = g_kernel_load_paddr + (KernelEntry - __vma_start);
    g_kernel_file_size = __file_end_lma - g_kernel_load_paddr;
    g_kernel_mem_size = __mem_end_lma - g_kernel_load_paddr;
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.eh_frame) }
}
