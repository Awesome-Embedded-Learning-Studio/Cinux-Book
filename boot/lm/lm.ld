ENTRY(LmEntry)
SECTIONS
{
    . = 0x9300;
    .text   : { *(.text.lm_entry) *(.text*) }
    .rodata : { *(.rodata*) }
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.eh_frame) }
}
ASSERT(ADDR(.text) == 0x9300, "lm blob must sit at the pinned entry VMA")
