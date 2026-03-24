/* ==============================================================
 * Cinux Mini Kernel - Main Entry Point
 * ============================================================== */

extern "C" {
#include <stdint.h>
}

// Forward declarations (provided by boot.S)
extern "C" {
    extern uint64_t __boot_info_ptr;
}

// Simple inline function for debugcon output
static void debugcon_putc(char c) {
    __asm__ volatile("outb %0, $0xE9" : : "a"(c));
}

extern "C" [[noreturn]] void mini_kernel_main(uint64_t boot_info_addr) {
    (void)boot_info_addr;  // Unused for now

    // Output 'M' to debugcon to verify we reached here
    debugcon_putc('M');

    // Halt
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}
