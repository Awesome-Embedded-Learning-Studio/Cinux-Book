/**
 * @file kernel/main.cpp
 * @brief Big kernel entry point
 *
 * This is the C++ main function for the "big kernel" -- the full-featured
 * kernel that the mini kernel loads from disk and jumps to.
 *
 * Milestone 010 goal:
 *   Trigger #BP exception via int $3, dump registers on serial, continue.
 *
 * The function:
 *   1. Initialises the serial port (COM1, 115200 8N1)
 *   2. Initialises the GDT (full: kernel + user + TSS)
 *   3. Initialises the IDT (CPU exception vectors 0-14)
 *   4. Prints the milestone message to confirm we reached here
 *   5. Triggers a software breakpoint (int $3) to test exception handling
 *   6. Enters an idle halt loop
 */

#include <stdint.h>

#include "kernel/arch/x86_64/gdt.hpp"
#include "kernel/arch/x86_64/idt.hpp"
#include "kernel/lib/kprintf.hpp"

// ============================================================
// Forward declaration (filled by linker / boot.S)
// ============================================================
// BootInfo is defined in boot/boot_info.h but for the current
// milestone we do not need it -- we just confirm the kernel runs.
// The full prototype will be: kernel_main(BootInfo* info)

/**
 * @brief Big kernel main entry point
 *
 * Called from boot.S after BSS clear and global ctors.
 *
 * @return This function should never return; the halt loop in
 *         boot.S catches it if it does.
 */
extern "C" void kernel_main() {
	// Step 1: Initialise the serial port used by kprintf
	cinux::lib::kprintf_init();

	// Step 2: Print the milestone message
	cinux::lib::kprintf("[BIG] Big kernel running @ 0x1000000\n");

	// Step 3: Initialise the GDT (must come before IDT)
	cinux::arch::g_gdt.init();
	cinux::lib::kprintf("[BIG] GDT loaded.\n");

	// Step 4: Initialise the IDT (depends on GDT selectors)
	cinux::arch::g_idt.init();
	cinux::lib::kprintf("[BIG] IDT loaded.\n");

	// Step 5: Trigger a software breakpoint to test exception handling
	// Note: do NOT enable interrupts (sti) yet -- we have no IRQ handlers
	// and a pending PIT timer would cause a Double Fault via unhandled IRQ.
	cinux::lib::kprintf("[BIG] Triggering int $3 breakpoint...\n");
	__asm__ volatile("int $3");
	cinux::lib::kprintf("[BIG] Breakpoint returned, continuing.\n");

	// Halt
	while (1) {
		__asm__ volatile("cli; hlt");
	}
}
