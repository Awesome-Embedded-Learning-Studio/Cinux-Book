#!/usr/bin/env bash
# scripts/build_fbdev_host_static.sh -- fbdev-host as a fully-static binary for
# the initramfs (no dynamic libc dependency). glibc-static if available, else
# the build fails and we switch to musl.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/build/smoke"
BUILD="$OUT/fbdev-static-build"
log() { printf '[fbdev-static] %s\n' "$*"; }
die() { log "FAIL: $*"; exit 1; }

log "build: fbdev-host (static) -- glibc-static path"
cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_FLAGS="-static -fno-exceptions -fno-rtti" \
  -DCMAKE_EXE_LINKER_FLAGS="-static" >/dev/null
cmake --build "$BUILD" -j"$(nproc)" --target fbdev-host >/dev/null
cp "$BUILD/fbdev-host" "$OUT/fbdev-host-static"
log "done: $(ls -lh "$OUT/fbdev-host-static" | awk '{print $5}')"
if file "$OUT/fbdev-host-static" 2>/dev/null | grep -q "statically linked\|static-pie"; then
  log "OK: statically linked"
else
  die "binary is NOT static (glibc-static missing?) -- install musl-tools and use musl-gcc"
fi
