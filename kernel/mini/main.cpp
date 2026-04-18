/* ==============================================================
 * Cinux Mini Kernel - Main Entry Point
 * ============================================================== */

extern "C" {
#include <stdint.h>
}

#include <boot_info.h>
#include "arch/x86_64/gdt.hpp"
#include "arch/x86_64/idt.hpp"
#include "lib/kprintf.h"
#include "mm/pmm.h"

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
	kprintf("BootInfo: entry_point=%p, kernel_phys_base=%p\n", boot_info->entry_point,
			boot_info->kernel_phys_base);
	kprintf("Boot Memory Info: mmap_count=%u\n", boot_info->mmap_count);
	for (uint32_t i = 0; i < boot_info->mmap_count; i++) {
		const MemoryMapEntry* entry = &boot_info->mmap[i];
		kprintf("  [%u] base=0x%016x, length=0x%016x, type=%u, acpi=%u\n", i, entry->base,
				entry->length, entry->type, entry->acpi);
	}

	// ============================================================
	// Initialize GDT (Global Descriptor Table)
	// ============================================================
	// GDT 必须在 IDT 之前初始化，因为 IDT 条目引用 GDT 中的代码段选择子
	kprintf("[INIT] Setting up GDT...\n");
	cinux::mini::arch::gdt_init();
	kprintf("[INIT] GDT loaded successfully.\n");

	// ============================================================
	// Initialize IDT (Interrupt Descriptor Table)
	// ============================================================
	// IDT 配置 #BP(3) 和 #PF(14) 两个异常向量
	kprintf("[INIT] Setting up IDT...\n");
	cinux::mini::arch::idt_init();
	kprintf("[INIT] IDT loaded successfully.\n");

	// ============================================================
	// Initialize Physical Memory Manager
	// ============================================================
	using cinux::mini::mm::pmm::init;
	init(boot_info);

	// ============================================================
	// 测试：触发 #BP(3) 断点异常
	// ============================================================
	// 通过 asm volatile("int $3") 手动触发断点异常，
	// 验证 GDT/IDT/ISR 是否正确配置。
	// 如果一切正常，handle_bp 会打印异常信息，然后返回这里继续执行。
	kprintf("\n[TEST] Triggering breakpoint exception (int $3)...\n");
	__asm__ volatile("int $3");
	kprintf("[TEST] Breakpoint test passed! Execution continued after #BP.\n\n");

	// Halt
	while (1) {
		__asm__ volatile("cli; hlt");
	}
}
