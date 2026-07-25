#!/usr/bin/env bash
#
# build-hello.sh — compile a static musl hello world against the Cinux sysroot.
#
# Links manually (-nostdlib) with the exact crt order musl-gcc.specs prescribes
# (Scrt1.o crti.o crtbeginS.o <objs> -lc -lgcc crtendS.o crtn.o).  We bypass the
# musl-gcc wrapper because GCC>=14 host specs inject -latomic_asneeded, which the
# wrapper does not suppress and which breaks the link.  crtbeginS/crtendS are
# GCC's own crt objects (handle .init_array/.fini_array/.eh_frame) — omitting
# them makes the binary segfault at startup.
#
# Usage: build-hello.sh [output-path]
#   default output: build/musl/hello
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
SYSROOT="${MUSL_SYSROOT:-$REPO/build/musl-sysroot}"
OUT="${1:-$REPO/build/musl/hello}"

if [ ! -f "$SYSROOT/lib/libc.a" ]; then
    echo "[build-hello] sysroot missing — run tools/musl/build-musl.sh first" >&2
    exit 1
fi

CB="$(gcc -print-file-name=crtbeginS.o)"
CE="$(gcc -print-file-name=crtendS.o)"
mkdir -p "$(dirname "$OUT")"

echo "[build-hello] compiling $HERE/hello.c -> $OUT"
gcc -static -nostdlib -no-pie \
    -L"$SYSROOT/lib" \
    "$SYSROOT/lib/Scrt1.o" "$SYSROOT/lib/crti.o" "$CB" \
    "$HERE/hello.c" \
    -lc -lgcc \
    "$CE" "$SYSROOT/lib/crtn.o" \
    -o "$OUT"

echo "[build-hello] artifact:"
file "$OUT"
