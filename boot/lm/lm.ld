ENTRY(LmEntry)
SECTIONS
{
    . = 0xB000;
    .text   : { *(.text.lm_entry) *(.text*) }
    .rodata : { *(.rodata*) }
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.eh_frame) }
}
ASSERT(ADDR(.text) == 0xB000, "lm blob must sit at the pinned entry VMA")
