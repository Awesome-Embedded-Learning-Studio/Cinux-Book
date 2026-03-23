#!/bin/bash
#
# scripts/build_image.sh
# @brief Build Cinux disk image with MBR and Stage2
#

set -e  # Exit on error

# ============================================================
# Path Configuration
# ============================================================

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
PROJECT_ROOT=$(dirname "$SCRIPT_DIR")

# ============================================================
# Parse Command Line Arguments
# ============================================================

# Usage: build_image.sh [mbr.bin] [stage2.bin] [output.img]
MBR_BIN=${1:-${BUILD_DIR}/boot/mbr.bin}
STAGE2_BIN=${2:-${BUILD_DIR}/boot/stage2.bin}
OUTPUT_IMAGE=${3:-${BUILD_DIR}/cinux.img}

# ============================================================
# Ensure Build Directory Exists
# ============================================================

mkdir -p "$(dirname "$OUTPUT_IMAGE")"

# ============================================================
# Validate Input Files
# ============================================================

# Check MBR binary exists
if [ ! -f "$MBR_BIN" ]; then
    echo "Error: MBR binary not found at $MBR_BIN"
    echo "Please run 'make' first to build the bootloader."
    exit 1
fi

# Check Stage2 binary exists
if [ ! -f "$STAGE2_BIN" ]; then
    echo "Error: Stage2 binary not found at $STAGE2_BIN"
    echo "Please run 'make' first to build the bootloader."
    exit 1
fi

# ============================================================
# Constants
# ============================================================

# Disk layout:
# Sector 0:     MBR (512 bytes)
# Sector 1-15:  Stage2 (up to 15 sectors = 7680 bytes)
STAGE2_LBA=1
STAGE2_MAX_SECTORS=15

# Get actual Stage2 size
STAGE2_SIZE=$(stat -c%s "$STAGE2_BIN" 2>/dev/null || stat -f%z "$STAGE2_BIN")
STAGE2_SECTORS=$(( (STAGE2_SIZE + 511) / 512 ))

# Validate Stage2 size
if [ $STAGE2_SECTORS -gt $STAGE2_MAX_SECTORS ]; then
    echo "Error: Stage2 too large: $STAGE2_SIZE bytes ($STAGE2_SECTORS sectors)"
    echo "       Maximum allowed: $STAGE2_MAX_SECTORS sectors ($((STAGE2_MAX_SECTORS * 512)) bytes)"
    exit 1
fi

# ============================================================
# Create Disk Image
# ============================================================

# Step 1: Create blank image (1MB = 2048 sectors)
# TODO: Adjust size based on actual needs
dd if=/dev/zero of="$OUTPUT_IMAGE" bs=1M count=1 status=none

# Step 2: Write MBR to sector 0
dd if="$MBR_BIN" of="$OUTPUT_IMAGE" bs=512 count=1 conv=notrunc status=none
echo "MBR written to sector 0"

# Step 3: Write Stage2 starting at sector 1
dd if="$STAGE2_BIN" of="$OUTPUT_IMAGE" bs=512 seek=$STAGE2_LBA conv=notrunc status=none
echo "Stage2 written to sectors $STAGE2_LBA-$((STAGE2_LBA + STAGE2_SECTORS - 1)) ($STAGE2_SECTORS sectors, $STAGE2_SIZE bytes)"

# ============================================================
# Verify Image
# ============================================================

# Verify MBR signature
SIGNATURE=$(dd if="$OUTPUT_IMAGE" bs=1 skip=510 count=2 status=none | xxd -p)
if [ "$SIGNATURE" = "55aa" ]; then
    echo "MBR signature valid: 0xAA55"
else
    echo "Warning: MBR signature invalid: $SIGNATURE (expected 55aa)"
fi

# ============================================================
# Output Result Information
# ============================================================

SIZE=$(stat -c%s "$OUTPUT_IMAGE" 2>/dev/null || stat -f%z "$OUTPUT_IMAGE")
echo ""
echo "Disk image built successfully!"
echo "  Path:  $OUTPUT_IMAGE"
echo "  Size:  $SIZE bytes"
echo ""
echo "To run Cinux:"
echo "  make run    # or"
echo "  qemu-system-x86_64 -drive file=$OUTPUT_IMAGE,format=raw"
