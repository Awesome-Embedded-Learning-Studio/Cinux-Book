/**
 * @file kernel/mini/driver/ata.cpp
 * @brief ATA PIO Mode Disk Driver Implementation
 *
 * Implements polling-based ATA PIO read operations for the mini kernel.
 * The driver communicates with the primary ATA controller (I/O ports 0x1F0-0x1F7,
 * control port 0x3F6) to read sectors from disk.
 */

#include "ata.hpp"

#include <stdint.h>
#include <stddef.h>

#include "driver/io.h"
#include "lib/kprintf.h"

using cinux::mini::lib::kprintf;

namespace cinux::mini::driver::ata {

// ============================================================
// Internal State
// ============================================================

/// Whether the ATA controller has been successfully initialized
static bool s_initialized = false;

namespace {

// ============================================================
// Internal I/O Helpers
// ============================================================

/**
 * @brief Read a byte from an ATA register
 * @param reg  Register offset from ATA_PRIMARY_BASE
 * @return Byte value read from the register
 */
inline uint8_t read_reg(uint16_t reg) {
	return io::inb(ATA_PRIMARY_BASE + reg);
}

/**
 * @brief Write a byte to an ATA register
 * @param reg    Register offset from ATA_PRIMARY_BASE
 * @param value  Byte value to write
 */
inline void write_reg(uint16_t reg, uint8_t value) {
	io::outb(ATA_PRIMARY_BASE + reg, value);
}

/**
 * @brief Read a 16-bit word from the ATA data register
 * @return 16-bit value from the data port
 */
inline uint16_t read_data() {
	uint16_t value;
	__asm__ volatile("inw %1, %0" : "=a"(value) : "Nd"(ATA_PRIMARY_BASE));
	return value;
}

// ============================================================
// Status Polling Helpers
// ============================================================

/**
 * @brief Wait until the ATA controller is not busy (BSY bit clear)
 *
 * Polls the status register until the BSY bit clears or a timeout
 * is reached. This is the first step in any ATA command sequence.
 *
 * @return true if the drive became ready, false on timeout
 */
bool wait_not_busy() {
	for (uint32_t i = 0; i < 10000000; i++) {
		uint8_t status = read_reg(ATA_REG_STATUS);
		if ((status & ATA_STATUS_BSY) == 0) {
			return true;
		}
		__asm__ volatile("pause");
	}
	return false;
}

/**
 * @brief Wait until the ATA data buffer is ready for reading (DRQ set, BSY clear)
 *
 * After issuing a read command and waiting the appropriate time,
 * this function polls until the drive asserts DRQ (data request),
 * indicating that 512 bytes of sector data are available at the data port.
 *
 * @return true if data is ready, false on error or timeout
 */
bool wait_data_ready() {
	for (uint32_t i = 0; i < 10000000; i++) {
		uint8_t status = read_reg(ATA_REG_STATUS);

		// Check for error conditions first
		if (status & ATA_STATUS_ERR) {
			kprintf("[ATA] ERROR: drive error, status=0x%02x, error=0x%02x\n", status,
					read_reg(ATA_REG_ERROR));
			return false;
		}
		if (status & ATA_STATUS_DF) {
			kprintf("[ATA] ERROR: drive fault, status=0x%02x\n", status);
			return false;
		}

		// Data is ready when BSY is clear and DRQ is set
		if ((status & ATA_STATUS_BSY) == 0 && (status & ATA_STATUS_DRQ)) {
			return true;
		}

		__asm__ volatile("pause");
	}

	kprintf("[ATA] ERROR: timeout waiting for data ready\n");
	return false;
}

/**
 * @brief Perform a 400ns delay by reading the alternate status register 4 times
 *
 * In ATA PIO mode, after issuing a command, the spec requires a 400ns delay
 * before polling the status register. Reading any ATA register takes ~100ns
 * on typical hardware, so 4 reads provide the required delay. We read the
 * control port to avoid clearing pending interrupt bits.
 */
void delay_400ns() {
	io::inb(ATA_PRIMARY_CTRL);
	io::inb(ATA_PRIMARY_CTRL);
	io::inb(ATA_PRIMARY_CTRL);
	io::inb(ATA_PRIMARY_CTRL);
}

}  // anonymous namespace

// ============================================================
// Initialization
// ============================================================

bool init() {
	kprintf("[INIT] Initializing ATA controller...\n");

	// Step 1: Perform software reset via the control register
	// Assert SRST (bit 1) and disable interrupts (bit 2 = nIEN)
	io::outb(ATA_PRIMARY_CTRL, 0x04);
	delay_400ns();

	// Deassert SRST, keep interrupts disabled
	io::outb(ATA_PRIMARY_CTRL, 0x00);
	delay_400ns();

	// Step 2: Wait for the drive to come out of reset
	if (!wait_not_busy()) {
		kprintf("[ATA] ERROR: drive did not come out of reset (BSY timeout)\n");
		return false;
	}

	// Step 3: Select the master drive with LBA mode
	write_reg(ATA_REG_DRIVE, ATA_DRIVE_MASTER);
	delay_400ns();

	// Step 4: Verify the drive is present and ready
	// After selecting the drive, wait for it to be ready
	if (!wait_not_busy()) {
		kprintf("[ATA] ERROR: master drive not ready after selection\n");
		return false;
	}

	uint8_t status = read_reg(ATA_REG_STATUS);

	// Check for floating bus (0xFF means no drive connected)
	if (status == 0xFF) {
		kprintf("[ATA] ERROR: no drive detected (floating bus)\n");
		return false;
	}

	// Check that the drive reports ready
	if ((status & ATA_STATUS_RDY) == 0) {
		kprintf("[ATA] ERROR: drive not ready, status=0x%02x\n", status);
		return false;
	}

	s_initialized = true;
	kprintf("[INIT] ATA controller initialized successfully (status=0x%02x).\n", status);
	return true;
}

// ============================================================
// Disk Read Operations
// ============================================================

bool read(uint64_t lba, uint16_t count, void* buffer) {
	// Step 1: Validate parameters
	if (!s_initialized) {
		kprintf("[ATA] ERROR: driver not initialized\n");
		return false;
	}
	if (count == 0) {
		kprintf("[ATA] ERROR: zero sector count\n");
		return false;
	}
	if (buffer == nullptr) {
		kprintf("[ATA] ERROR: null buffer\n");
		return false;
	}
	if (lba >= (1ULL << 48)) {
		kprintf("[ATA] ERROR: LBA out of 48-bit range\n");
		return false;
	}

	// Step 2: Wait for the drive to not be busy
	if (!wait_not_busy()) {
		kprintf("[ATA] ERROR: drive busy before read\n");
		return false;
	}

	// Step 3: Determine addressing mode and send command
	bool use_lba48 = (lba >= 0x10000000ULL) || (count > 256);

	if (use_lba48) {
		// LBA48 addressing: send high 16 bits of LBA and high byte of count first
		write_reg(ATA_REG_DRIVE, ATA_DRIVE_MASTER | 0x40);  // LBA48 mode bit
		delay_400ns();

		// High order bytes (sent first, per ATA spec)
		write_reg(ATA_REG_SECTOR_CNT, static_cast<uint8_t>((count >> 8) & 0xFF));
		write_reg(ATA_REG_LBA_LOW, static_cast<uint8_t>((lba >> 24) & 0xFF));
		write_reg(ATA_REG_LBA_MID, static_cast<uint8_t>((lba >> 32) & 0xFF));
		write_reg(ATA_REG_LBA_HIGH, static_cast<uint8_t>((lba >> 40) & 0xFF));

		// Low order bytes (sent second)
		write_reg(ATA_REG_SECTOR_CNT, static_cast<uint8_t>(count & 0xFF));
		write_reg(ATA_REG_LBA_LOW, static_cast<uint8_t>(lba & 0xFF));
		write_reg(ATA_REG_LBA_MID, static_cast<uint8_t>((lba >> 8) & 0xFF));
		write_reg(ATA_REG_LBA_HIGH, static_cast<uint8_t>((lba >> 16) & 0xFF));

		// Issue READ SECTORS EXT command
		write_reg(ATA_REG_COMMAND, ATA_CMD_READ_PIO_EXT);
	} else {
		// LBA28 addressing
		write_reg(ATA_REG_DRIVE,
				  ATA_DRIVE_MASTER | static_cast<uint8_t>((lba >> 24) & 0x0F));
		delay_400ns();

		write_reg(ATA_REG_SECTOR_CNT, static_cast<uint8_t>(count & 0xFF));
		write_reg(ATA_REG_LBA_LOW, static_cast<uint8_t>(lba & 0xFF));
		write_reg(ATA_REG_LBA_MID, static_cast<uint8_t>((lba >> 8) & 0xFF));
		write_reg(ATA_REG_LBA_HIGH, static_cast<uint8_t>((lba >> 16) & 0xFF));

		// Issue READ SECTORS command
		write_reg(ATA_REG_COMMAND, ATA_CMD_READ_PIO);
	}

	// Step 4: Read sectors one by one
	auto* buf = static_cast<uint16_t*>(buffer);

	for (uint16_t sector = 0; sector < count; sector++) {
		// Wait for the sector data to be ready
		delay_400ns();
		if (!wait_data_ready()) {
			kprintf("[ATA] ERROR: failed reading sector %u (LBA %u)\n", sector,
					static_cast<uint32_t>(lba + sector));
			return false;
		}

		// Read 256 x 16-bit words (512 bytes) from the data port
		for (int word = 0; word < 256; word++) {
			buf[word] = read_data();
		}

		buf += 256;  // Advance by 512 bytes (256 words)
	}

	return true;
}

}  // namespace cinux::mini::driver::ata
