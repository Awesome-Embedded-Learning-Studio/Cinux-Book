#!/bin/bash
# Create a small ext2 disk image for QEMU AHCI port 1
#
# Usage: ./create_ext2_disk.sh <output_image> [shell_elf] [musl_hello_elf]
#
# Creates a 4 MB ext2 filesystem image with:
#   /bin/sh         - Shell executable (from shell_elf argument)
#   /etc/motd       - Welcome message
#   /hello.txt      - Simple text file
#   /hello          - musl static hello world (F10-M1 batch 6, only if 3rd arg
#                     names an existing file; enables the run-kernel-test
#                     ring-3 musl smoke)
#
# Uses debugfs from e2fsprogs to populate the filesystem without
# requiring root/mount permissions.

set -e

OUTPUT="$1"
SHELL_ELF="$2"
MUSL_HELLO_ELF="$3"

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

# F10-M1 batch 6: optional musl static hello world at /hello. Only included
# when the caller passes a path that exists (musl sysroot built separately by
# tools/musl/build-musl.sh + build-hello.sh; absent in CI, which skips the
# ring-3 smoke test).
if [ -n "$MUSL_HELLO_ELF" ] && [ -f "$MUSL_HELLO_ELF" ]; then
    DEBUGFS_CMDS+="write $MUSL_HELLO_ELF hello\n"
fi

printf "$DEBUGFS_CMDS" | debugfs -w "$OUTPUT" >/dev/null 2>&1

echo "Created ext2 image: $OUTPUT ($IMAGE_SIZE MB, block_size=$BLOCK_SIZE)"
