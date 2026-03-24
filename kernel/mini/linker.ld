/* ==============================================================
 * Cinux Mini Kernel - Minimal Linker Script
 * ==============================================================

 * Physical load address: 0x200000 (2MB)
 * This ELF is used for testing disk header reading.
 */

OUTPUT_FORMAT("elf64-x86-64")
ENTRY(_start)

SECTIONS
{
    /* Kernel loads at physical 2MB */
    . = 0x200000;

    .text : {
        *(.text)
    }

    .rodata : {
        *(.rodata)
    }

    .data : {
        *(.data)
    }

    .bss : {
        *(.bss)
        *(COMMON)
    }

    /DISCARD/ : {
        *(.comment*)
        *(.note*)
        *(.eh_frame*)
    }
}
