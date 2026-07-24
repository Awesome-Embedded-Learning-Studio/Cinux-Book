#!/usr/bin/env bash
#
# build-input-event-test.sh — compile the static musl /dev/event0 input smoke.
#
# Mirrors build-hello.sh / build-fb-mmap-test.sh (manual -nostdlib crt link; see
# build-hello.sh for why we bypass the musl-gcc wrapper).  Produces a static
# PIE-less ELF the kernel smoke harness execve's at /input_event_test to
# exercise the batch-2 /dev/event0 read path (ISR push -> ring -> copy_to_user).
#
# Usage: build-input-event-test.sh [output-path]
#   default output: build/musl/input_event_test
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
SYSROOT="${MUSL_SYSROOT:-$REPO/build/musl-sysroot}"
OUT="${1:-$REPO/build/musl/input_event_test}"

if [ ! -f "$SYSROOT/lib/libc.a" ]; then
    echo "[build-input-event-test] sysroot missing — run tools/musl/build-musl.sh first" >&2
    exit 1
fi

CB="$(gcc -print-file-name=crtbeginS.o)"
CE="$(gcc -print-file-name=crtendS.o)"
mkdir -p "$(dirname "$OUT")"

echo "[build-input-event-test] compiling $HERE/input_event_test.c -> $OUT"
gcc -static -nostdlib -no-pie \
    -L"$SYSROOT/lib" \
    "$SYSROOT/lib/Scrt1.o" "$SYSROOT/lib/crti.o" "$CB" \
    "$HERE/input_event_test.c" \
    -lc -lgcc \
    "$CE" "$SYSROOT/lib/crtn.o" \
    -o "$OUT"

echo "[build-input-event-test] artifact:"
file "$OUT"
