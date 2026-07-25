#!/usr/bin/env bash
#
# build-fb-mmap-test.sh — compile the static musl /dev/fb0 mmap smoke.
#
# Mirrors build-hello.sh (manual -nostdlib crt link; see build-hello.sh for why
# we bypass the musl-gcc wrapper).  Produces a static PIE-less ELF the kernel
# smoke harness execve's at /fb_mmap_test to exercise the IoPhys VMA path.
#
# Usage: build-fb-mmap-test.sh [output-path]
#   default output: build/musl/fb_mmap_test
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
SYSROOT="${MUSL_SYSROOT:-$REPO/build/musl-sysroot}"
OUT="${1:-$REPO/build/musl/fb_mmap_test}"

if [ ! -f "$SYSROOT/lib/libc.a" ]; then
    echo "[build-fb-mmap-test] sysroot missing — run tools/musl/build-musl.sh first" >&2
    exit 1
fi

CB="$(gcc -print-file-name=crtbeginS.o)"
CE="$(gcc -print-file-name=crtendS.o)"
mkdir -p "$(dirname "$OUT")"

echo "[build-fb-mmap-test] compiling $HERE/fb_mmap_test.c -> $OUT"
gcc -static -nostdlib -no-pie \
    -L"$SYSROOT/lib" \
    "$SYSROOT/lib/Scrt1.o" "$SYSROOT/lib/crti.o" "$CB" \
    "$HERE/fb_mmap_test.c" \
    -lc -lgcc \
    "$CE" "$SYSROOT/lib/crtn.o" \
    -o "$OUT"

echo "[build-fb-mmap-test] artifact:"
file "$OUT"
