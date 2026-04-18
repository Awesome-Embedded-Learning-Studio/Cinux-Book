/**
 * @file kernel/mini/big_kernel_loader.cpp
 * @brief Big Kernel Loader Implementation
 *
 * Orchestrates the loading of the big kernel from disk through the
 * ATA driver and ELF parser. This is the high-level loading pipeline
 * that ties the subsystems together.
 */

#include "big_kernel_loader.hpp"

#include <stdint.h>

#include "driver/ata.hpp"
#include "elf_loader.hpp"
#include "lib/kprintf.h"

using cinux::mini::lib::kprintf;

namespace cinux::mini::loader {

// ============================================================
// Big Kernel Loading
// ============================================================

uint64_t load_big_kernel(uint64_t disk_lba) {
	// Step 1: Log the start of the loading process
	kprintf("[LOADER] Loading big kernel from disk LBA 0x%x...\n", disk_lba);

	constexpr uint32_t staging_bytes =
		static_cast<uint32_t>(BIG_KERNEL_MAX_SECTORS) * driver::ata::ATA_SECTOR_SIZE;
	kprintf("[LOADER] Staging at physical address 0x%p (%u KB buffer)\n",
			BIG_KERNEL_LOAD_ADDR, staging_bytes / 1024);

	// Step 2: Read the big kernel ELF binary from disk into the staging buffer
	if (!driver::ata::read(disk_lba, BIG_KERNEL_MAX_SECTORS,
						   reinterpret_cast<void*>(BIG_KERNEL_LOAD_ADDR))) {
		kprintf("[LOADER] ERROR: Failed to read big kernel from disk!\n");
		return 0;
	}

	kprintf("[LOADER] Read %u sectors (%u KB) from disk.\n", BIG_KERNEL_MAX_SECTORS,
			staging_bytes / 1024);

	// Step 3: Quick sanity check on the first bytes of the staging buffer
	const auto* magic = reinterpret_cast<const uint8_t*>(BIG_KERNEL_LOAD_ADDR);
	if (magic[0] != 0x7F || magic[1] != 'E' || magic[2] != 'L' || magic[3] != 'F') {
		kprintf("[LOADER] ERROR: No ELF magic at staging buffer! Got: %02x %02x %02x %02x\n",
				magic[0], magic[1], magic[2], magic[3]);
		return 0;
	}

	kprintf("[LOADER] ELF magic verified at staging buffer.\n");

	// Step 4: Load the ELF binary using the ELF loader.
	// Pass the staging buffer size so the loader can verify that segment data
	// (p_offset + p_filesz) does not exceed what we actually read from disk.
	uint64_t entry = elf_loader::load_elf(
		reinterpret_cast<void*>(BIG_KERNEL_LOAD_ADDR), staging_bytes);
	if (entry == 0) {
		kprintf("[LOADER] ERROR: ELF loading failed!\n");
		return 0;
	}

	// Step 5: Log the entry point and return it
	kprintf("[LOADER] Big kernel loaded successfully.\n");
	kprintf("[LOADER] Entry point: 0x%p\n", entry);
	return entry;
}

}  // namespace cinux::mini::loader
