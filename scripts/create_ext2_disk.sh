#!/bin/bash
# Create an ext2 disk image for QEMU AHCI port 1.
#
# Usage: create_ext2_disk.sh <output_image> [shell_elf] [musl_hello_elf]
#                            [musl_forktest_elf] [musl_hello_dyn_elf]
#                            [musl_ldso_elf] [busybox_elf] [gcc_root_dir]
#
# Builds the filesystem from a staging directory tree with `mkfs.ext2 -d` (B4-B1
# replaced the per-file `debugfs write/ln` script, which could not scale to the
# ~10k GCC headers). `mkfs.ext2 -d` copies the whole tree in one pass AND keeps
# hardlinks (busybox applet hardlinks collapse to one inode), verified on
# e2fsprogs 1.47.4.
#
# Contents (conditional on which ELF args name existing files):
#   /bin/sh              - busybox (ash applet) or the user_shell ELF
#   /bin/busybox         - static musl busybox + applet hardlinks (ash/ls/cat/...)
#   /sbin/init           - busybox hardlink (init applet, PID1, B3b)
#   /etc/{motd,passwd,group,inittab}, /hello.txt
#   /hello, /forktest    - musl static smoke ELFs (F10-M1 / F-VERIFY M5-2)
#   /hello-dyn           - musl dynamic hello (F10-M2)
#   /lib/ld-musl-x86_64.so.1 - musl ldso, PT_INTERP target (F10-M2)
#   <gcc_root>/*         - GCC toolchain subset (B4-b: as/ld + glibc runtime +
#                          crt + libgcc; staged by tools/gcc-toolchain/extract.sh)
#
# Requires e2fsprogs mkfs.ext2 with -d (root-directory) support, 1.43+.

set -e

OUTPUT="$1"
SHELL_ELF="$2"
MUSL_HELLO_ELF="$3"
MUSL_FORKTEST_ELF="$4"
MUSL_HELLO_DYN_ELF="$5"
MUSL_LDSO_ELF="$6"
BUSYBOX_ELF="$7"
FB_MMAP_TEST_ELF="$8"
INPUT_EVENT_TEST_ELF="$9"
CINUX_GUI_HOST_ELF="${10}"
GCC_ROOT="${11}"

if [ -z "$OUTPUT" ]; then
    echo "Usage: $0 <output_image> [shell_elf] ..." >&2
    exit 1
fi

if ! command -v mkfs.ext2 &>/dev/null; then
    echo "Error: mkfs.ext2 not found (install e2fsprogs)" >&2
    exit 1
fi

# 8 MB holds busybox + the musl smoke ELFs. Callers staging the GCC toolchain
# (B4-b) bump IMAGE_SIZE=128 + INODES=8192 via the environment.
IMAGE_SIZE="${IMAGE_SIZE:-8}"
BLOCK_SIZE=1024
INODES="${INODES:-1024}"

ROOT=$(mktemp -d)
trap "rm -rf $ROOT" EXIT

mkdir -p "$ROOT/etc" "$ROOT/bin"

cat > "$ROOT/etc/motd" << 'MOTD_EOF'
Welcome to Cinux!
This message was read from an ext2 filesystem.
MOTD_EOF

# F-ECO batch 8: minimal /etc/passwd + /etc/group so whoami/id resolve uid 0.
cat > "$ROOT/etc/passwd" << 'PASSWD_EOF'
root::0:0:root:/:/bin/sh
PASSWD_EOF
cat > "$ROOT/etc/group" << 'GROUP_EOF'
root:x:0:
GROUP_EOF

# B3b busybox init: /etc/inittab consumed by the init applet (PID1). Minimal --
# a sysinit banner then respawn /bin/sh on the console.
cat > "$ROOT/etc/inittab" << 'INITTAB_EOF'
::sysinit:/bin/echo Cinux init: filesystems mounted
::respawn:/bin/sh
INITTAB_EOF

cat > "$ROOT/hello.txt" << 'HELLO_EOF'
Hello from ext2!
Cinux can read files from a real filesystem now.
HELLO_EOF

# /bin/sh: prefer busybox (argv[0] /bin/sh -> basename "sh" -> ash shell), else
# fall back to the user_shell ELF (CI / no sysroot).
if [ -n "$BUSYBOX_ELF" ] && [ -f "$BUSYBOX_ELF" ]; then
    cp -p "$BUSYBOX_ELF" "$ROOT/bin/busybox"
    # Applet hardlinks: argv[0] basename dispatches to the applet, and hardlinks
    # keep one inode (mkfs -d preserves them, so the image stays small).
    BUSYBOX_APPLETS="
sh ash ls clear cat echo pwd uname id whoami true false sleep env hostname wc free ps
mkdir rmdir touch ln chmod chown rm cp mv readlink
"
    for applet in $BUSYBOX_APPLETS; do
        ln "$ROOT/bin/busybox" "$ROOT/bin/$applet"
    done
    # B3b: /sbin/init -> busybox hardlink (argv[0] basename "init" -> init applet).
    mkdir -p "$ROOT/sbin"
    ln "$ROOT/bin/busybox" "$ROOT/sbin/init"
elif [ -n "$SHELL_ELF" ] && [ -f "$SHELL_ELF" ]; then
    cp -p "$SHELL_ELF" "$ROOT/bin/sh"
fi

# F10-M1 batch 6: optional musl static hello at /hello (ring-3 smoke).
if [ -n "$MUSL_HELLO_ELF" ] && [ -f "$MUSL_HELLO_ELF" ]; then
    cp -p "$MUSL_HELLO_ELF" "$ROOT/hello"
fi
# F-VERIFY M5-2: optional musl static forktest at /forktest (SMP CoW reproducer).
if [ -n "$MUSL_FORKTEST_ELF" ] && [ -f "$MUSL_FORKTEST_ELF" ]; then
    cp -p "$MUSL_FORKTEST_ELF" "$ROOT/forktest"
fi
# F-GUI-USERSPACE batch 1b: optional musl static /dev/fb0 mmap smoke (IoPhys VMA).
if [ -n "$FB_MMAP_TEST_ELF" ] && [ -f "$FB_MMAP_TEST_ELF" ]; then
    cp -p "$FB_MMAP_TEST_ELF" "$ROOT/fb_mmap_test"
fi
# F-GUI-USERSPACE batch 2: optional musl static /dev/event0 input smoke.
if [ -n "$INPUT_EVENT_TEST_ELF" ] && [ -f "$INPUT_EVENT_TEST_ELF" ]; then
    cp -p "$INPUT_EVENT_TEST_ELF" "$ROOT/input_event_test"
fi
# F-GUI-USERSPACE batch 3a: optional musl static userspace GUI host.
if [ -n "$CINUX_GUI_HOST_ELF" ] && [ -f "$CINUX_GUI_HOST_ELF" ]; then
    cp -p "$CINUX_GUI_HOST_ELF" "$ROOT/cinux_gui_host"
fi
# F10-M2: optional musl dynamic hello + its interpreter at the PT_INTERP path.
if [ -n "$MUSL_HELLO_DYN_ELF" ] && [ -f "$MUSL_HELLO_DYN_ELF" ] && \
   [ -n "$MUSL_LDSO_ELF" ] && [ -f "$MUSL_LDSO_ELF" ]; then
    mkdir -p "$ROOT/lib"
    cp -p "$MUSL_LDSO_ELF" "$ROOT/lib/ld-musl-x86_64.so.1"
    cp -p "$MUSL_HELLO_DYN_ELF" "$ROOT/hello-dyn"
fi

# B4-B1: optional GCC toolchain subset (staged by tools/gcc-toolchain/extract.sh).
# cp -a preserves permissions AND symlinks (SONAME symlinks like libbfd.so ->
# libbfd-2.46.0.so must stay symlinks for ldso DT_NEEDED resolution). The tree's
# /usr, /lib64, /usr/lib layout matches GCC's built-in specs (hardcoded paths).
if [ -n "$GCC_ROOT" ] && [ -d "$GCC_ROOT" ]; then
    cp -a "$GCC_ROOT"/. "$ROOT"/
fi

dd if=/dev/zero of="$OUTPUT" bs=1M count="$IMAGE_SIZE" 2>/dev/null
mkfs.ext2 -q -b "$BLOCK_SIZE" -O none -N "$INODES" -d "$ROOT" "$OUTPUT" >/dev/null 2>&1

echo "Created ext2 image: $OUTPUT ($IMAGE_SIZE MB, block_size=$BLOCK_SIZE, inodes=$INODES, mkfs -d)"
