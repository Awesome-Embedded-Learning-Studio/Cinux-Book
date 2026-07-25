#!/usr/bin/env bash
#
# build-cinux-exit.sh — compile the cinux-exit helper statically against the
# Cinux musl sysroot, into the Buildroot overlay (rootfs/overlay/sbin/) so
# Buildroot (BR2_ROOTFS_OVERLAY) packs it into rootfs.ext2. cinux-exit triggers
# sys_cinux_exit (=221, Cinux-custom) -> QEMU isa-debug-exit, gating the
# buildroot-usability CI job.
#
# Links manually (-nostdlib) with the exact crt order musl-gcc.specs prescribes,
# bypassing the musl-gcc wrapper (GCC>=14 host specs inject -latomic_asneeded,
# which the wrapper does not suppress and which breaks the link) — same trick as
# build-hello.sh.
#
# Usage: build-cinux-exit.sh
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
SYSROOT="${MUSL_SYSROOT:-$REPO/build/musl-sysroot}"
OUT_DIR="$REPO/rootfs/overlay/sbin"
OUT="$OUT_DIR/cinux-exit"

if [ ! -f "$SYSROOT/lib/libc.a" ]; then
    echo "[build-cinux-exit] sysroot missing — run tools/musl/build-musl.sh first" >&2
    exit 1
fi

CB="$(gcc -print-file-name=crtbeginS.o)"
CE="$(gcc -print-file-name=crtendS.o)"
mkdir -p "$OUT_DIR"

echo "[build-cinux-exit] compiling $HERE/cinux-exit.c -> $OUT"
gcc -static -nostdlib -no-pie \
    -L"$SYSROOT/lib" \
    "$SYSROOT/lib/Scrt1.o" "$SYSROOT/lib/crti.o" "$CB" \
    "$HERE/cinux-exit.c" \
    -lc -lgcc \
    "$CE" "$SYSROOT/lib/crtn.o" \
    -o "$OUT"

echo "[build-cinux-exit] artifact:"
file "$OUT"
