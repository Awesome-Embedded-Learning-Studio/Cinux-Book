#!/usr/bin/env bash
#
# build-hello-dyn.sh — compile a DYNAMIC musl hello world against the CinuxOS
# sysroot. This is the F10-M2 smoke binary: a non-PIE dynamic executable
# (ET_EXEC) carrying PT_INTERP = /lib/ld-musl-x86_64.so.1, so the kernel's
# new PT_INTERP path loads musl's ldso, which relocates the program in user
# space. Mirrors build-hello.sh (manual -nostdlib link; the musl-gcc wrapper
# is broken on GCC>=14) but drops -static and sets the dynamic linker.
#
# Usage: build-hello-dyn.sh [output-path]
#   default output: build/musl/hello-dyn
#
# Host-run needs /lib/ld-musl-x86_64.so.1 installed (this script does NOT do
# that -- the host is glibc). readelf confirms the binary is well-formed; the
# actual run is the QEMU ring-3 smoke (F10-M2 batch 3), where the interp is
# installed on the ext2 image at /lib/ld-musl-x86_64.so.1.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
SYSROOT="${MUSL_SYSROOT:-$REPO/build/musl-sysroot}"
OUT="${1:-$REPO/build/musl/hello-dyn}"

if [ ! -f "$SYSROOT/lib/libc.so" ]; then
    echo "[build-hello-dyn] sysroot/lib/libc.so missing — run tools/musl/build-musl.sh first" >&2
    exit 1
fi

CB="$(gcc -print-file-name=crtbeginS.o)"
CE="$(gcc -print-file-name=crtendS.o)"
mkdir -p "$(dirname "$OUT")"

# Dynamic, non-PIE: ET_EXEC + PT_INTERP. -nostdlib + manual crt (same crt order
# as the static link); -lc now resolves to libc.so (the dynamic musl libc /
# ldso). -dynamic-linker bakes PT_INTERP = the path the kernel reads.
echo "[build-hello-dyn] compiling $HERE/hello.c -> $OUT"
gcc -nostdlib -no-pie \
    -L"$SYSROOT/lib" \
    "$SYSROOT/lib/Scrt1.o" "$SYSROOT/lib/crti.o" "$CB" \
    "$HERE/hello.c" \
    -lc -lgcc \
    "$CE" "$SYSROOT/lib/crtn.o" \
    -Wl,-dynamic-linker,/lib/ld-musl-x86_64.so.1 \
    -o "$OUT"

echo "[build-hello-dyn] artifact:"
file "$OUT"
echo "[build-hello-dyn] program headers (expect ET_EXEC + INTERP + PT_PHDR):"
readelf -hl "$OUT" | sed -n '1,40p'
