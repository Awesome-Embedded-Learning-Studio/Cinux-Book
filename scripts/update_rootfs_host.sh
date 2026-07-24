#!/bin/bash
# update_rootfs_host.sh — overwrite /cinux_gui_host inside the production rootfs
# ext2 in-place via debugfs, without re-running assemble_gcc_rootfs.sh (which
# restages the whole GCC closure). Invoked by the cmake `update-rootfs-host`
# target, which `run` depends on, so editing core/host sources + `cmake --build
# --target run` always ships the fresh host.
#
# Usage: update_rootfs_host.sh <rootfs.ext2> <host_elf>
set -euo pipefail

ROOTFS="${1:?usage: $0 <rootfs.ext2> <host_elf>}"
HOST="${2:?usage: $0 <rootfs.ext2> <host_elf>}"

if [ ! -f "$ROOTFS" ]; then
    echo "[update-rootfs-host] rootfs not found: $ROOTFS" >&2
    exit 1
fi
if [ ! -f "$HOST" ]; then
    echo "[update-rootfs-host] host ELF not found: $HOST" >&2
    exit 1
fi

# debugfs `write` refuses to clobber an existing inode, so remove first then
# write. `rm` is tolerated if the host is not yet present (first run).
debugfs -w "$ROOTFS" <<EOF >/dev/null 2>&1
rm /cinux_gui_host
write $HOST /cinux_gui_host
quit
EOF

echo "[update-rootfs-host] /cinux_gui_host refreshed in $ROOTFS"
