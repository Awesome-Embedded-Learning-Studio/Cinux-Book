/* ==============================================================
 * Cinux Mini Kernel - Main Entry Point
 * ============================================================== */

extern "C" {
#include <stdint.h>
}

#include "../../boot/boot_info.h"
#include "lib/kprintf.h"

using cinux::mini::lib::kprintf;

extern "C" {
extern uint64_t __boot_info_ptr;
}

extern "C" [[noreturn]] void mini_kernel_main(uint64_t boot_info_addr) {
    BootInfo* boot_info = (BootInfo*)__boot_info_ptr;
    (void)boot_info_addr;

    // ============================================================
    // Kernel Entry Point
    // ============================================================
    kprintf("Cinux Mini Kernel v0.1.0\n");
    kprintf("BootInfo: entry_point=%p, kernel_phys_base=%p\n",
            boot_info->entry_point, boot_info->kernel_phys_base);

    // TODO: Initialize kernel subsystems
    // TODO: Start scheduler

    // Halt
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}
