ENTRY(stage2_start)
SECTIONS
{
    . = 0x7E00;
    .text   : { *(.text.boot) *(.text*) }
    .rodata : { *(.rodata*) }
    .data   : { *(.data*) }
    .lm_blob 0xB000 : { KEEP(*(.lm_blob)) }
    .bss    : { *(.bss*) *(COMMON) }
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
