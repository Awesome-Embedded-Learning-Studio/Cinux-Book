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
    .rodata : ALIGN(16) { *(.rodata*) }
    .data : ALIGN(16) { *(.data*) }
    .bss : ALIGN(16) {
        g_kernel_bss_start = .;
        *(.bss*)
        *(COMMON)
        g_kernel_bss_end = .;
    }
    __vma_end = .;
    __lma_start = LOADADDR(.image_header);
    __file_end_lma = SIZEOF(.data) ? LOADADDR(.data) + SIZEOF(.data) :
                    (SIZEOF(.rodata) ? LOADADDR(.rodata) + SIZEOF(.rodata) :
                     LOADADDR(.text) + SIZEOF(.text));
    __mem_end_lma = __lma_start + (__vma_end - __vma_start);
    __entry_lma = __lma_start + (KernelEntry - __vma_start);
    __header_file_size = __file_end_lma - __lma_start;
    __header_mem_size = __mem_end_lma - __lma_start;
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.eh_frame) }
}
