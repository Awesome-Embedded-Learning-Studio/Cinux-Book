ENTRY(mbr_start)
SECTIONS
{
    # Put it here, we assume we start here
    . = 0x7C00;
    .text   : { *(.text.boot) *(.text*) }
    .rodata : { *(.rodata*) }
    .data   : { *(.data*) }
    .bss    : { *(.bss*) *(COMMON) }
    _mbr_end = .;
    ASSERT(_mbr_end <= 0x7C00 + 510, "MBR exceeds 510 bytes")
    . = 0x7C00 + 510;
    .sig    : { SHORT(0xAA55) }
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}