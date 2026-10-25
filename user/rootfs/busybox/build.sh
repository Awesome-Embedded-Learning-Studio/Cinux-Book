#!/usr/bin/env bash
# One door to the glibc user system: cmake owns the whole chain (kernel,
# user programs, glibc busybox with its fingerprinted cache, the volume),
# and this wrapper only picks the exit. `run` boots interactive KVM,
# `build` assembles the images, `verify` fires the non-interactive
# ecosystem verdict. BusyBox download, checksum and cache live inside
# scripts/build-busybox-glibc.sh, so the cmake targets reach them too.
# This file's home is user/rootfs/busybox/, three levels under the repo.
set -euo pipefail

repo="$(cd "$(dirname "$0")/../../.." && pwd)"
cd "$repo"
action="${1:-run}"
case "$action" in build|run|verify) ;; *) echo "usage: $0 [build|run|verify]" >&2; exit 2;; esac
export CINUX_BUILD_DIR="${CINUX_BUILD_DIR:-${repo}/build}"
export CINUX_ROOTFS_JOBS="${CINUX_ROOTFS_JOBS:-4}"

cmake -S . -B "$CINUX_BUILD_DIR"
cmake --build "$CINUX_BUILD_DIR" --parallel "$CINUX_ROOTFS_JOBS" --target boot_image ext2_img

if [ "$action" = "verify" ]; then
    python3 scripts/test_station33_ecosystem.py --image "${CINUX_BUILD_DIR}/boot/cinux.img"
    exit 0
fi
if [ "$action" = "run" ]; then
    test -c /dev/kvm && test -w /dev/kvm || { echo 'KVM is required and must be writable' >&2; exit 1; }
    cmake --build "$CINUX_BUILD_DIR" --target run
fi
