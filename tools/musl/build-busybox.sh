#!/usr/bin/env bash
#
# build-busybox.sh -- build the static BusyBox used by the CinuxOS smoke.
#
# Produces build/musl/busybox by default.  The checked-in config is copied from
# the known-good local BusyBox build that drove the F-ECO applet work.
set -euo pipefail

BUSYBOX_VER="${BUSYBOX_VER:-1.36.0}"
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
SYSROOT="${MUSL_SYSROOT:-$REPO/build/musl-sysroot}"
WORK="$REPO/build/busybox-src"
SRC="$WORK/busybox-$BUSYBOX_VER"
TARBALL="$WORK/busybox-$BUSYBOX_VER.tar.bz2"
OUT="${1:-$REPO/build/musl/busybox}"
CC="${BUSYBOX_CC:-$SYSROOT/bin/musl-gcc}"
CONFIG_FILE="${BUSYBOX_CONFIG:-$HERE/busybox-$BUSYBOX_VER.config}"

if [ -x "$OUT" ]; then
    echo "[build-busybox] busybox already present at $OUT (rm to rebuild)"
    exit 0
fi

if [ ! -x "$CC" ]; then
    echo "[build-busybox] compiler missing: $CC" >&2
    echo "[build-busybox] run tools/musl/build-musl.sh first" >&2
    exit 1
fi

if [ ! -f "$CONFIG_FILE" ]; then
    echo "[build-busybox] config missing: $CONFIG_FILE" >&2
    exit 1
fi

mkdir -p "$WORK" "$(dirname "$OUT")"

# build-musl.sh patches musl-gcc with -fno-link-libatomic for GCC versions that
# inject -latomic_asneeded on static links.  Older CI GCCs do not recognize that
# option and fail during BusyBox compile-only steps.  Keep the cached sysroot
# intact; use a local wrapper copy without the flag when this host compiler does
# not accept it.
if ! printf 'int x;\n' | "${REALGCC:-gcc}" -fno-link-libatomic -x c -c -o /tmp/cinux-gcc-flag-test.o - >/dev/null 2>&1; then
    if grep -q -- '-fno-link-libatomic' "$CC"; then
        SANITIZED_CC="$WORK/musl-gcc-no-link-libatomic"
        sed 's/ -fno-link-libatomic//g' "$CC" >"$SANITIZED_CC"
        chmod +x "$SANITIZED_CC"
        CC="$SANITIZED_CC"
        echo "[build-busybox] host gcc lacks -fno-link-libatomic; using sanitized musl-gcc wrapper"
    fi
fi

if [ ! -f "$TARBALL" ]; then
    echo "[build-busybox] downloading busybox-$BUSYBOX_VER..."
    curl -fsSL "https://busybox.net/downloads/busybox-$BUSYBOX_VER.tar.bz2" -o "$TARBALL"
fi

if [ ! -d "$SRC" ]; then
    echo "[build-busybox] extracting..."
    tar -C "$WORK" -xjf "$TARBALL"
fi

cd "$SRC"
echo "[build-busybox] using config $CONFIG_FILE"
cp "$CONFIG_FILE" .config

make -s oldconfig </dev/null >/dev/null

echo "[build-busybox] building with $(nproc) jobs..."
make -j"$(nproc)" CC="$CC" busybox
install -m 0755 busybox "$OUT"

echo "[build-busybox] artifact:"
file "$OUT"
