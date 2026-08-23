#!/usr/bin/env bash
# scripts/build_busybox_x86_64.sh -- static busybox for the P1 initramfs.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="${BUSYBOX_SRC:-/home/charliechen/PenguinLab/third_party/busybox}"
OUT="$ROOT/build/smoke/busybox-build"
INSTALL="$ROOT/build/smoke/busybox-root"
log() { printf '[busybox] %s\n' "$*"; }
die() { log "FAIL: $*"; exit 1; }
[[ -f "$SRC/Makefile" ]] || die "busybox source not found: $SRC (set BUSYBOX_SRC)"
mkdir -p "$OUT"
cd "$SRC"
if [[ ! -f "$OUT/.config" ]]; then
  log "config: defconfig"
  make O="$OUT" defconfig
fi
# static binary (initramfs has no libc)
sed -i 's/^# CONFIG_STATIC is not set/CONFIG_STATIC=y/' "$OUT/.config" || true
grep -q '^CONFIG_STATIC=y' "$OUT/.config" || echo 'CONFIG_STATIC=y' >> "$OUT/.config"
sed -i 's/^CONFIG_PIE=y/# CONFIG_PIE is not set/' "$OUT/.config" || true
# CONFIG_TC: busybox 1.37 tc.c uses CBQ uapi removed in kernel 6.19 headers -> compile error
sed -i 's/^CONFIG_TC=y/# CONFIG_TC is not set/' "$OUT/.config" || true
grep -q '^# CONFIG_TC is not set' "$OUT/.config" || echo '# CONFIG_TC is not set' >> "$OUT/.config"
make O="$OUT" olddefconfig 2>/dev/null || yes "" | make O="$OUT" oldconfig >/dev/null 2>&1 || true
log "build: busybox (-j$(nproc)) -- Wno-error (1.37 + gcc16 -Werror)"
make O="$OUT" -j"$(nproc)" EXTRA_CFLAGS="-Wno-error" busybox
rm -rf "$INSTALL"
make O="$OUT" CONFIG_PREFIX="$INSTALL" install
log "done: $(ls -lh "$INSTALL/bin/busybox" | awk '{print $5}')"
