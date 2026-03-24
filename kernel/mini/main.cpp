/* ==============================================================
 * Cinux Mini Kernel - Minimal ELF for Disk Load Testing
 * ==============================================================

 * Absolute minimal ELF to test bootloader disk reading.
 * Just halts.
 */

extern "C" {
[[noreturn]] void _start() {
	while (1) {
		__asm__ volatile(
			"cli; \
            hlt");
	}
}
}
