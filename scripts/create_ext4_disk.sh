#!/bin/bash
# Create a small ext4 (extents) disk image for QEMU AHCI port 2.
#
# Usage: ./create_ext4_disk.sh <output_image>
#
# Builds an ext4 filesystem with the extents feature and populates two files
# used by the F6-M5 ext4-extent read-path mechanism test (test_ext4_extents.cpp):
#   /big.bin   - 1 MiB regular file, byte[i] = i & 0xFF.  At 1 KB blocks this is
#                1024 logical blocks mapped by a single depth-0 leaf extent,
#                exercising multi-block reads within one extent.
#   /small.txt - short text file, a single-block leaf extent.
#
# Layout constraint: this kernel's ext2 driver reads the block group descriptor
# table with a fixed 32-byte stride (it ignores s_desc_size), so the image MUST
# use classic 32-byte descriptors.  We therefore disable 64bit + metadata_csum
# (which would grow descriptors to 64 bytes) and assert the result.
#
# Uses mkfs.ext4 / debugfs / dumpe2fs from e2fsprogs and python3 to generate the
# deterministic byte pattern.  No root/mount required.

set -e

OUTPUT="$1"
if [ -z "$OUTPUT" ]; then
    echo "Usage: $0 <output_image>" >&2
    exit 1
fi

for tool in mkfs.ext4 debugfs dumpe2fs python3; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "Error: $tool not found (install e2fsprogs / python3)" >&2
        exit 1
    fi
done

IMAGE_SIZE=8   # MB
BLOCK_SIZE=1024

# ext4 with extents, but forced back to 32-byte group descriptors: ^64bit keeps
# s_desc_size == 0 (classic descriptors), ^metadata_csum avoids the 64-byte
# checksummed descriptor variant.  extents stays on so debugfs-written files
# become extent-mapped (EXT4_EXTENTS_FL) inodes.
FEATURES="extents,^64bit,^metadata_csum"

dd if=/dev/zero of="$OUTPUT" bs=1M count="$IMAGE_SIZE" status=none
mkfs.ext4 -t ext4 -b "$BLOCK_SIZE" -O "$FEATURES" -N 128 -F "$OUTPUT" >/dev/null 2>&1

# --- Layout sanity: fail loudly if the feature set is not what mount() expects.
FEATS="$(dumpe2fs -h "$OUTPUT" 2>/dev/null | awk -F: '/^Filesystem features/ {print $2}')"
check_feat() {  # check_feat <needle> <want_present> <label>
    case "$FEATS" in
        *"$1"*) [ "$2" = "present" ] && return 0 ;;
        *)      [ "$2" = "absent"  ] && return 0 ;;
    esac
    echo "Error: ext4 feature check failed: '$1' should be $2 ($3)" >&2
    exit 1
}
check_feat "extent"        "present" "extents must be enabled for the test"
check_feat "64bit"         "absent"  "64bit grows group descriptors to 64 bytes"
check_feat "metadata_csum" "absent"  "metadata_csum grows group descriptors to 64 bytes"

# --- Populate.  Pattern file + small text file.
TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

python3 -c "import sys; sys.stdout.buffer.write(bytes(i & 0xFF for i in range(1048576)))" \
    > "$TMPDIR/big.bin"
printf 'ext4 extents small file\n' > "$TMPDIR/small.txt"

debugfs -w "$OUTPUT" >/dev/null 2>&1 <<EOF
write $TMPDIR/big.bin big.bin
write $TMPDIR/small.txt small.txt
EOF

echo "Created ext4 (extents) image: $OUTPUT (${IMAGE_SIZE} MB, block_size=${BLOCK_SIZE})"
