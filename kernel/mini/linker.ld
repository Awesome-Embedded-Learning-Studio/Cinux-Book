/* ==============================================================
 * Cinux Mini Kernel - Minimal Linker Script
 * ==============================================================

 * Physical load address: 0x20000 (128KB)
 * Flat binary: objcopy -O binary removes all ELF headers
 */

OUTPUT_FORMAT("elf64-x86-64")
ENTRY(_start)

SECTIONS
{
    /* Kernel loads at physical 0x20000 (128KB) */
    . = 0x20000;

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
        __bss_start = .;
        *(.bss)
        *(COMMON)
        __bss_end = .;
    }

    /DISCARD/ : {
        *(.comment*)
        *(.note*)
        *(.eh_frame*)
    }
}
