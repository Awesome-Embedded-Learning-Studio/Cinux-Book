#!/usr/bin/env bash
#
# build-forktest.sh — compile the SMP CoW-race reproducer (forktest.c) as a musl
# static binary against the CinuxOS sysroot.  Mirrors build-hello.sh (same manual
# -nostdlib crt link order; see build-hello.sh for why we bypass musl-gcc).
#
# F-VERIFY M5-2: packed as /forktest on the ext2 smoke image so the ring-3 smoke
# can execve it under -smp 2 and gate on `FORKTEST races=0`.  forktest exercises
# the REAL user-task fork + CoW-write path that the F10 fixes guard.
#
# Usage: build-forktest.sh [output-path]
#   default output: build/musl/forktest
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
SYSROOT="${MUSL_SYSROOT:-$REPO/build/musl-sysroot}"
OUT="${1:-$REPO/build/musl/forktest}"
# forktest.c defaults to 300 iterations (the F10 deep-repro setting); that is
# slow under TCG -smp and can exceed the run-kernel-test timeout.  Default the
# GATE binary to 50 (fast + still a real CoW stress; F10 crashed at ITERS>=2
# pre-fix).  Override FORKTEST_ITERS for a manual deep dig (e.g. 300).
ITERS="${FORKTEST_ITERS:-50}"

if [ ! -f "$SYSROOT/lib/libc.a" ]; then
    echo "[build-forktest] sysroot missing — run tools/musl/build-musl.sh first" >&2
    exit 1
fi

CB="$(gcc -print-file-name=crtbeginS.o)"
CE="$(gcc -print-file-name=crtendS.o)"
mkdir -p "$(dirname "$OUT")"

echo "[build-forktest] compiling $HERE/forktest.c (ITERS=$ITERS) -> $OUT"
gcc -static -nostdlib -no-pie \
    -DFORKTEST_ITERS="$ITERS" \
    -L"$SYSROOT/lib" \
    "$SYSROOT/lib/Scrt1.o" "$SYSROOT/lib/crti.o" "$CB" \
    "$HERE/forktest.c" \
    -lc -lgcc \
    "$CE" "$SYSROOT/lib/crtn.o" \
    -o "$OUT"

echo "[build-forktest] artifact:"
file "$OUT"
