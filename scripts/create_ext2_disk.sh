#!/bin/bash
# Create a small ext2 disk image for QEMU AHCI port 1
#
# Usage: ./create_ext2_disk.sh <output_image> [shell_elf]
#
# Creates a 4 MB ext2 filesystem image with:
#   /bin/sh         - Shell executable (from shell_elf argument)
#   /etc/motd       - Welcome message
#   /hello.txt      - Simple text file
#
# Uses debugfs from e2fsprogs to populate the filesystem without
# requiring root/mount permissions.

set -e

OUTPUT="$1"
SHELL_ELF="$2"

if [ -z "$OUTPUT" ]; then
    echo "Usage: $0 <output_image> [shell_elf]" >&2
    exit 1
fi

# Check for required tools
if ! command -v mkfs.ext2 &>/dev/null; then
    echo "Error: mkfs.ext2 not found (install e2fsprogs)" >&2
    exit 1
fi

if ! command -v debugfs &>/dev/null; then
    echo "Error: debugfs not found (install e2fsprogs)" >&2
    exit 1
fi

IMAGE_SIZE=4  # MB
BLOCK_SIZE=1024

# Create a zero-filled image
dd if=/dev/zero of="$OUTPUT" bs=1M count="$IMAGE_SIZE" 2>/dev/null

# Format as ext2
mkfs.ext2 -b "$BLOCK_SIZE" -O none -N 128 "$OUTPUT" >/dev/null 2>&1

# Create temporary files for content
TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

mkdir -p "$TMPDIR/etc"

cat > "$TMPDIR/etc/motd" << 'MOTD_EOF'
Welcome to Cinux!
This message was read from an ext2 filesystem.
MOTD_EOF

cat > "$TMPDIR/hello.txt" << 'HELLO_EOF'
Hello from ext2!
Cinux can read files from a real filesystem now.
HELLO_EOF

# Build the debugfs command script
DEBUGFS_CMDS=""
DEBUGFS_CMDS+="mkdir etc\n"
DEBUGFS_CMDS+="write $TMPDIR/etc/motd etc/motd\n"
DEBUGFS_CMDS+="write $TMPDIR/hello.txt hello.txt\n"

if [ -n "$SHELL_ELF" ] && [ -f "$SHELL_ELF" ]; then
    DEBUGFS_CMDS+="mkdir bin\n"
    DEBUGFS_CMDS+="write $SHELL_ELF bin/sh\n"
fi

printf "$DEBUGFS_CMDS" | debugfs -w "$OUTPUT" >/dev/null 2>&1

echo "Created ext2 image: $OUTPUT ($IMAGE_SIZE MB, block_size=$BLOCK_SIZE)"
