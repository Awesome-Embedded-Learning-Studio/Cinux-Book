#!/bin/bash
# Create a small ext2 disk image for QEMU AHCI port 1
#
# Usage: ./create_ext2_disk.sh <output_image> [shell_elf] [musl_hello_elf] [musl_forktest_elf]
#
# Creates a 4 MB ext2 filesystem image with:
#   /bin/sh         - Shell executable (from shell_elf argument)
#   /etc/motd       - Welcome message
#   /hello.txt      - Simple text file
#   /hello          - musl static hello world (F10-M1 batch 6, only if 3rd arg
#                     names an existing file; enables the run-kernel-test
#                     ring-3 musl smoke)
#   /forktest       - musl static SMP CoW-race reproducer (F-VERIFY M5-2, only
#                     if 4th arg names an existing file; the ring-3 smoke execve's
#                     it under -smp 2 and gates on `FORKTEST races=0`)
#   /hello-dyn      - musl DYNAMIC hello world (F10-M2, only if 5th arg names an
#                     existing file); non-PIE ET_EXEC with PT_INTERP, exercises
#                     the kernel's interpreter-loading path
#   /lib/ld-musl-x86_64.so.1 - musl dynamic linker / shared libc (F10-M2, only
#                     if 6th arg names an existing file); the PT_INTERP target
#                     the kernel loads for a dynamic executable
#
# Uses debugfs from e2fsprogs to populate the filesystem without
# requiring root/mount permissions.

set -e

OUTPUT="$1"
SHELL_ELF="$2"
MUSL_HELLO_ELF="$3"
MUSL_FORKTEST_ELF="$4"
MUSL_HELLO_DYN_ELF="$5"
MUSL_LDSO_ELF="$6"
BUSYBOX_ELF="$7"

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

IMAGE_SIZE=8  # MB (holds the 822 KB musl ldso + test ELFs)
# 1024-byte blocks (the ext2 default). The kernel driver now resolves direct
# + single-indirect + double-indirect block pointers (see ext2_common.cpp
# Ext2FileOps::read and ext2_inode.cpp Ext2::get_or_alloc_block), so files
# beyond the ~268 KB single-indirect ceiling -- e.g. the 822 KB musl ldso --
# are read/written through i_block[13] instead of being truncated. Earlier
# the image used 4096-byte blocks as a workaround to push that ceiling to
# ~4 MB; real double-indirect support (F10-M2 follow-up) makes that unnecessary.
# block_buf_[4096] in ext2.hpp still accommodates the max ext2 block size.
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

# F-ECO batch 8 busybox acceptance: minimal /etc/passwd + /etc/group so
# whoami/id resolve uid 0 -> "root" (without these busybox whoami exits 1).
cat > "$TMPDIR/etc/passwd" << 'PASSWD_EOF'
root::0:0:root:/:/bin/sh
PASSWD_EOF
cat > "$TMPDIR/etc/group" << 'GROUP_EOF'
root:x:0:
GROUP_EOF

# B3b busybox init: /etc/inittab consumed by the init applet (PID1).  Minimal --
# a sysinit banner then respawn /bin/sh on the console.  getty/login and real
# runlevels are a staged follow-up.
cat > "$TMPDIR/etc/inittab" << 'INITTAB_EOF'
::sysinit:/bin/echo CinuxOS init: filesystems mounted
::respawn:/bin/sh
INITTAB_EOF

# Build the debugfs command script
DEBUGFS_CMDS=""
DEBUGFS_CMDS+="mkdir etc\n"
DEBUGFS_CMDS+="mkdir bin\n"
DEBUGFS_CMDS+="write $TMPDIR/etc/motd etc/motd\n"
DEBUGFS_CMDS+="write $TMPDIR/etc/passwd etc/passwd\n"
DEBUGFS_CMDS+="write $TMPDIR/etc/group etc/group\n"
DEBUGFS_CMDS+="write $TMPDIR/etc/inittab etc/inittab\n"
DEBUGFS_CMDS+="write $TMPDIR/hello.txt hello.txt\n"

# /bin/sh: prefer busybox (interactive `sh` applet) when built -- argv[0]
# "/bin/sh" → busybox dispatches by basename "sh" → ash shell, so the existing
# shell_launch (which execves /bin/sh) boots straight into busybox sh for the
# smoke test. Absent busybox → fall back to the user_shell ELF (CI / no sysroot).
if [ -n "$BUSYBOX_ELF" ] && [ -f "$BUSYBOX_ELF" ]; then
    DEBUGFS_CMDS+="write $BUSYBOX_ELF bin/sh\n"
elif [ -n "$SHELL_ELF" ] && [ -f "$SHELL_ELF" ]; then
    DEBUGFS_CMDS+="write $SHELL_ELF bin/sh\n"
fi

# F10-M1 batch 6: optional musl static hello world at /hello. Only included
# when the caller passes a path that exists (musl sysroot built separately by
# tools/musl/build-musl.sh + build-hello.sh; absent in CI, which skips the
# ring-3 smoke test).
if [ -n "$MUSL_HELLO_ELF" ] && [ -f "$MUSL_HELLO_ELF" ]; then
    DEBUGFS_CMDS+="write $MUSL_HELLO_ELF hello\n"
fi

# F-VERIFY M5-2: optional SMP CoW-race reproducer at /forktest (musl static;
# built by tools/musl/build-forktest.sh).  The ring-3 smoke execve's it under
# -smp 2 to gate the F10 fork/CoW fixes.  Absent in CI (no sysroot) -> skipped.
if [ -n "$MUSL_FORKTEST_ELF" ] && [ -f "$MUSL_FORKTEST_ELF" ]; then
    DEBUGFS_CMDS+="write $MUSL_FORKTEST_ELF forktest\n"
fi

# F10-M2: optional musl DYNAMIC hello at /hello-dyn (non-PIE ET_EXEC + PT_INTERP,
# built by tools/musl/build-hello-dyn.sh) + its interpreter at the PT_INTERP
# path /lib/ld-musl-x86_64.so.1 (the musl shared libc / ldso from the sysroot).
# The ring-3 dyn smoke execve's /hello-dyn to exercise the kernel's interpreter
# load path. Absent in CI (no sysroot) -> both skipped.
if { [ -n "$MUSL_HELLO_DYN_ELF" ] && [ -f "$MUSL_HELLO_DYN_ELF" ]; } && \
   { [ -n "$MUSL_LDSO_ELF" ] && [ -f "$MUSL_LDSO_ELF" ]; }; then
    DEBUGFS_CMDS+="mkdir lib\n"
    # Install the interpreter (musl libc.so) at the exact PT_INTERP path, then
    # the dynamic hello. debugfs `write` copies bytes, so this is a real file
    # (not a symlink) -- fine, the kernel reads it via inode->ops->read.
    DEBUGFS_CMDS+="write $MUSL_LDSO_ELF lib/ld-musl-x86_64.so.1\n"
    DEBUGFS_CMDS+="write $MUSL_HELLO_DYN_ELF hello-dyn\n"
fi

# F-ECO batch 0: optional busybox at /bin/busybox (static musl).  Install common
# applet hard links too: interactive ash resolves "ls" via PATH=/bin to
# /bin/ls, not to "/bin/busybox ls".  Hard links keep the image small and mirror
# the normal BusyBox deployment model.
if [ -n "$BUSYBOX_ELF" ] && [ -f "$BUSYBOX_ELF" ]; then
    DEBUGFS_CMDS+="write $BUSYBOX_ELF bin/busybox\n"
    BUSYBOX_APPLETS="
ash
ls
clear
cat
echo
pwd
uname
id
whoami
true
false
sleep
env
hostname
wc
free
ps
mkdir
rmdir
touch
ln
chmod
chown
rm
cp
mv
readlink
"
    busybox_links=1
    for applet in $BUSYBOX_APPLETS; do
        DEBUGFS_CMDS+="ln /bin/busybox /bin/$applet\n"
        busybox_links=$((busybox_links + 1))
    done
    # B3b: /sbin/init -> busybox hardlink.  argv[0] "/sbin/init" -> basename
    # "init" -> busybox init applet (CONFIG_INIT=y), which reads /etc/inittab
    # and respawns /bin/sh as a PID1 child.  kernel_init_thread execves this
    # path directly (shell_launch.cpp launch_userspace).
    DEBUGFS_CMDS+="mkdir sbin\n"
    DEBUGFS_CMDS+="ln /bin/busybox /sbin/init\n"
    busybox_links=$((busybox_links + 1))
    DEBUGFS_CMDS+="sif /bin/busybox links_count $busybox_links\n"
fi


printf "$DEBUGFS_CMDS" | debugfs -w "$OUTPUT" >/dev/null 2>&1

echo "Created ext2 image: $OUTPUT ($IMAGE_SIZE MB, block_size=$BLOCK_SIZE)"
