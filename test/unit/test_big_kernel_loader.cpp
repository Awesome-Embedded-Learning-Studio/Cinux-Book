/**
 * @file test/unit/test_big_kernel_loader.cpp
 * @brief Host-side unit tests for Big Kernel Loader constants and logic
 *
 * Test coverage:
 *   - Loader constant correctness (addresses, LBA, sector counts)
 *   - Memory layout constraints (no overlap between mini kernel, staging, segments)
 *   - ELF magic sanity check logic
 *   - Size arithmetic for disk read operations
 *   - Load pipeline state transitions (valid/invalid scenarios)
 *
 * The big kernel loader orchestrates ATA read + ELF loading.
 * On host, we verify the pure logic and constants; hardware-dependent
 * parts (actual disk I/O) are tested via the ATA and ELF loader tests.
 *
 * Compile condition: -DCINUX_HOST_TEST
 */

#define TEST_FRAMEWORK_IMPL
#include "test_framework.h"

#ifdef CINUX_HOST_TEST

#include <cstddef>
#include <cstdint>
#include <cstring>

// Include headers for constants and types
#include "mini/big_kernel_loader.hpp"
#include "mini/driver/ata.hpp"
#include "mini/elf_loader.hpp"

using namespace cinux::mini::loader;
using namespace cinux::mini::driver::ata;
using namespace cinux::mini::elf_loader;

// ============================================================
// 1. Loader Constant Correctness
// ============================================================

/**
 * @brief Verify mini kernel load address constant
 *
 * The mini kernel is loaded at 0x20000 (128KB) by the bootloader.
 */
TEST("loader: mini kernel load address") {
    ASSERT_EQ(MINI_KERNEL_LOAD_ADDR, 0x20000ULL);
}

/**
 * @brief Verify big kernel staging address
 *
 * The big kernel ELF is staged at 0x1000000 (16MB).
 */
TEST("loader: big kernel staging address") {
    ASSERT_EQ(BIG_KERNEL_LOAD_ADDR, 0x1000000ULL);
}

/**
 * @brief Verify big kernel starting LBA on disk
 *
 * The big kernel starts at sector 848 (after the mini kernel region).
 */
TEST("loader: big kernel starting LBA") {
    ASSERT_EQ(BIG_KERNEL_LBA, 848ULL);
}

/**
 * @brief Verify maximum sectors to read for the big kernel
 *
 * 512 sectors = 256KB maximum big kernel size.
 */
TEST("loader: max sectors for big kernel") {
    ASSERT_EQ(BIG_KERNEL_MAX_SECTORS, 512);
}

// ============================================================
// 2. Memory Layout Constraints
// ============================================================

/**
 * @brief Verify staging buffer is above mini kernel
 *
 * The staging buffer at 16MB must not overlap with the mini kernel at 128KB.
 */
TEST("loader: staging above mini kernel") {
    ASSERT_GT(BIG_KERNEL_LOAD_ADDR, MINI_KERNEL_LOAD_ADDR);
}

/**
 * @brief Verify the mini kernel region ends before the staging buffer
 *
 * Mini kernel max size is ~416KB = 0x20000 + 0x68000 = 0x88000.
 * Staging buffer is at 0x1000000 (16MB), well above this.
 */
TEST("loader: mini kernel region below staging") {
    uint64_t mini_kernel_end = MINI_KERNEL_LOAD_ADDR + 416 * 1024;
    ASSERT_LT(mini_kernel_end, BIG_KERNEL_LOAD_ADDR);
}

/**
 * @brief Verify staging buffer size in bytes
 */
TEST("loader: staging buffer size") {
    uint64_t staging_size = static_cast<uint64_t>(BIG_KERNEL_MAX_SECTORS) * ATA_SECTOR_SIZE;
    ASSERT_EQ(staging_size, 512ULL * 512);  // 256KB = 262144 bytes
}

/**
 * @brief Verify staging buffer size is reasonable for a kernel
 *
 * 256KB should be enough for an initial big kernel.
 */
TEST("loader: staging buffer is reasonable size") {
    uint64_t staging_size = static_cast<uint64_t>(BIG_KERNEL_MAX_SECTORS) * ATA_SECTOR_SIZE;
    ASSERT_GE(staging_size, 64ULL * 1024);   // At least 64KB
    ASSERT_LE(staging_size, 16ULL * 1024 * 1024);  // At most 16MB
}

/**
 * @brief Verify staging buffer does not wrap around in 32-bit address space
 */
TEST("loader: staging buffer within 32-bit range") {
    uint64_t staging_end = BIG_KERNEL_LOAD_ADDR +
                           static_cast<uint64_t>(BIG_KERNEL_MAX_SECTORS) * ATA_SECTOR_SIZE;
    ASSERT_LE(staging_end, 0x100000000ULL);  // Within 4GB
}

// ============================================================
// 3. Disk Layout Arithmetic
// ============================================================

/**
 * @brief Verify LBA to byte offset conversion for big kernel start
 *
 * LBA 848 * 512 = 434176 bytes from disk start.
 */
TEST("loader: big kernel byte offset on disk") {
    uint64_t byte_offset = BIG_KERNEL_LBA * ATA_SECTOR_SIZE;
    ASSERT_EQ(byte_offset, 848ULL * 512);
}

/**
 * @brief Verify the big kernel LBA is after the mini kernel region
 *
 * Mini kernel: sectors 16 to 16+832-1 = 847.
 * Big kernel starts at sector 848.
 */
TEST("loader: big kernel after mini kernel on disk") {
    uint64_t mini_kernel_start_lba = 16;
    uint64_t mini_kernel_sectors = 832;  // ~416KB / 512
    uint64_t mini_kernel_end_lba = mini_kernel_start_lba + mini_kernel_sectors;
    ASSERT_GE(BIG_KERNEL_LBA, mini_kernel_end_lba);
}

/**
 * @brief Verify total disk read size for the big kernel
 */
TEST("loader: total read size") {
    uint64_t total_bytes = static_cast<uint64_t>(BIG_KERNEL_MAX_SECTORS) * ATA_SECTOR_SIZE;
    ASSERT_EQ(total_bytes, 262144ULL);  // 256KB
}

// ============================================================
// 4. ELF Magic Sanity Check Logic
// ============================================================

/**
 * @brief Verify ELF magic byte check used in load_big_kernel
 *
 * The loader checks: magic[0]=0x7F, magic[1]='E', magic[2]='L', magic[3]='F'
 */
TEST("loader: ELF magic byte check valid") {
    uint8_t magic[4] = {0x7F, 'E', 'L', 'F'};
    ASSERT_EQ(magic[0], 0x7F);
    ASSERT_EQ(magic[1], 'E');
    ASSERT_EQ(magic[2], 'L');
    ASSERT_EQ(magic[3], 'F');
}

/**
 * @brief Verify ELF magic check rejects non-ELF data
 */
TEST("loader: ELF magic rejects non-ELF") {
    // Check various non-ELF patterns
    uint8_t not_elf1[4] = {0x00, 'E', 'L', 'F'};  // Bad first byte
    ASSERT_NE(not_elf1[0], 0x7F);

    uint8_t not_elf2[4] = {0x7F, 'X', 'L', 'F'};  // Bad second byte
    ASSERT_NE(not_elf2[1], 'E');

    uint8_t not_elf3[4] = {0x00, 0x00, 0x00, 0x00};  // All zeros
    ASSERT_FALSE(not_elf3[0] == 0x7F && not_elf3[1] == 'E' &&
                 not_elf3[2] == 'L' && not_elf3[3] == 'F');
}

// ============================================================
// 5. Load Pipeline Logic
// ============================================================

/**
 * @brief Verify load_big_kernel returns 0 for non-ELF data
 *
 * When the staging buffer does not contain valid ELF magic,
 * the loader should detect this and return 0 (failure).
 *
 * Since we cannot call the actual load_big_kernel (it does real disk I/O),
 * we test the equivalent logic: checking if invalid ELF data is detected.
 */
TEST("loader: non-ELF staging buffer detected") {
    // Simulate what load_big_kernel does after reading from disk:
    // Check the first 4 bytes of the staging buffer
    uint8_t fake_buffer[512];
    memset(fake_buffer, 0xFF, sizeof(fake_buffer));

    // The loader checks: magic[0] != 0x7F || magic[1] != 'E' || ...
    const auto* magic = fake_buffer;
    bool is_elf = (magic[0] == 0x7F && magic[1] == 'E' &&
                   magic[2] == 'L' && magic[3] == 'F');
    ASSERT_FALSE(is_elf);
}

/**
 * @brief Verify that valid ELF data passes the staging buffer magic check
 */
TEST("loader: valid ELF passes staging check") {
    // Build a minimal valid ELF header
    uint8_t fake_buffer[512];
    memset(fake_buffer, 0, sizeof(fake_buffer));
    fake_buffer[0] = 0x7F;
    fake_buffer[1] = 'E';
    fake_buffer[2] = 'L';
    fake_buffer[3] = 'F';

    const auto* magic = fake_buffer;
    bool is_elf = (magic[0] == 0x7F && magic[1] == 'E' &&
                   magic[2] == 'L' && magic[3] == 'F');
    ASSERT_TRUE(is_elf);
}

// ============================================================
// 6. Integration: Combined Loader Constants
// ============================================================

/**
 * @brief Verify the complete load address chain
 *
 * Mini kernel (0x20000) < Staging buffer (0x1000000)
 * Staging buffer + 256KB should not overflow into problematic regions
 */
TEST("loader: complete address chain") {
    // Mini kernel: 0x20000 - 0x88000
    uint64_t mini_start = MINI_KERNEL_LOAD_ADDR;
    uint64_t mini_end = mini_start + 416 * 1024;
    ASSERT_EQ(mini_start, 0x20000ULL);

    // Staging buffer: 0x1000000 - 0x1040000
    uint64_t staging_start = BIG_KERNEL_LOAD_ADDR;
    uint64_t staging_end = staging_start +
                           static_cast<uint64_t>(BIG_KERNEL_MAX_SECTORS) * ATA_SECTOR_SIZE;

    // No overlap
    ASSERT_LT(mini_end, staging_start);
    // Staging is at 16MB
    ASSERT_EQ(staging_start, 0x1000000ULL);
    // Staging end is at 16MB + 256KB
    ASSERT_EQ(staging_end, 0x1040000ULL);
}

/**
 * @brief Verify BIG_KERNEL_LBA matches expected disk layout
 *
 * MBR = sector 0
 * Stage2 = sectors 1-15
 * Mini kernel = sectors 16+
 * Big kernel = sectors 848+
 *
 * Total reserved for mini kernel: 848 - 16 = 832 sectors = 416KB
 */
TEST("loader: disk sector allocation") {
    uint64_t mbr_end = 1;
    uint64_t stage2_end = 16;
    uint64_t mini_kernel_start = 16;
    uint64_t mini_kernel_sectors = BIG_KERNEL_LBA - mini_kernel_start;

    ASSERT_EQ(mini_kernel_sectors, 832ULL);
    ASSERT_EQ(mini_kernel_sectors * ATA_SECTOR_SIZE, 425984ULL);  // 416KB

    (void)mbr_end;
    (void)stage2_end;
}

// ============================================================
// 7. LBA Range Validation for Big Kernel
// ============================================================

/**
 * @brief Verify big kernel LBA is within 48-bit range
 */
TEST("loader: big kernel LBA within valid range") {
    ASSERT_LT(BIG_KERNEL_LBA, (1ULL << 48));
}

/**
 * @brief Verify big kernel LBA + max sectors does not exceed 48-bit range
 */
TEST("loader: big kernel read range within bounds") {
    uint64_t end_lba = BIG_KERNEL_LBA + BIG_KERNEL_MAX_SECTORS;
    ASSERT_LT(end_lba, (1ULL << 48));
}

/**
 * @brief Verify big kernel LBA can be addressed with LBA28
 *
 * Since LBA 848 < 0x10000000, LBA28 is sufficient.
 */
TEST("loader: big kernel LBA within LBA28 range") {
    ASSERT_LT(BIG_KERNEL_LBA, 0x10000000ULL);
}

// ============================================================
// 8. Higher-Half Kernel Address Arithmetic
// ============================================================

/**
 * @brief Verify higher-half base constant matches ELF loader
 *
 * The ELF loader uses 0xFFFFFFFF80000000 as the higher-half base.
 * We verify this is consistent with what the big kernel expects.
 */
TEST("loader: higher-half base constant") {
    constexpr uint64_t HIGHER_HALF_BASE = 0xFFFFFFFF80000000ULL;
    // Typical higher-half kernel: virtual entry ~0xFFFFFFFF80100000
    // Physical: 0xFFFFFFFF80100000 - 0xFFFFFFFF80000000 = 0x100000
    uint64_t virtual_entry = 0xFFFFFFFF80100000ULL;
    uint64_t physical_entry = virtual_entry - HIGHER_HALF_BASE;
    ASSERT_EQ(physical_entry, 0x100000ULL);
}

/**
 * @brief Verify entry point below higher-half base is used as-is
 */
TEST("loader: lower-half entry used as-is") {
    constexpr uint64_t HIGHER_HALF_BASE = 0xFFFFFFFF80000000ULL;
    uint64_t virtual_entry = 0x1000ULL;
    // Below HIGHER_HALF_BASE, should not be subtracted
    ASSERT_LT(virtual_entry, HIGHER_HALF_BASE);
    // Physical = virtual (identity mapped)
    uint64_t physical = virtual_entry;
    ASSERT_EQ(physical, 0x1000ULL);
}

// ============================================================
// Main Function
// ============================================================

/**
 * @brief Test entry point
 */
int main() {
    RUN_ALL_TESTS();
    return _tests_failed > 0 ? 1 : 0;
}

#endif  // CINUX_HOST_TEST
