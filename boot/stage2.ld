ENTRY(stage2_start)
SECTIONS
{
    . = 0x7E00;
    .text   : { *(.text.boot) *(.text*) }
    .rodata : { *(.rodata*) }
    .data   : { *(.data*) }
    .bss    : { *(.bss*) *(COMMON) }
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
