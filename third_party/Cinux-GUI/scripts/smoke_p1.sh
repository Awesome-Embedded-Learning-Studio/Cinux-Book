#!/usr/bin/env bash
# scripts/smoke_p1.sh -- P1 fbdev-host smoke with a SELF-BUILT x86_64 kernel +
# busybox+fbdev-host initramfs. The initramfs carries everything (busybox +
# static fbdev-host + /init), so there is NO guest->host transport (no 9p, no
# HTTP, no network) -- this is why we moved off the Alpine ISO approach.
#
# /init (in the initramfs) is run by the kernel (rdinit=/init); it mounts
# proc/sys/devtmpfs, discovers /dev/fb0 + the usb-tablet event node, and runs
# "timeout 40 fbdev-host /dev/fb0 /dev/input/eventN". All its output goes to
# /dev/ttyS0 -> the captured serial log -> the host gates grep GUEST_* markers.
#
# Prereqs (build once, cached):
#   scripts/build_kernel_x86_64.sh      -> build/smoke/linux-build/.../bzImage
#   scripts/build_busybox_x86_64.sh     -> build/smoke/busybox-root/
#   scripts/build_fbdev_host_static.sh  -> build/smoke/fbdev-host-static
#   scripts/build_initramfs.sh          -> build/smoke/rootfs.cpio.gz
#
# Usage:  ./scripts/smoke_p1.sh
# Exit:   0 PASS (boot + devices + fbdev-host loop + no-crash), 1 FAIL/error.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SMOKE="$ROOT/build/smoke"
BZIMAGE="$SMOKE/linux-build/arch/x86/boot/bzImage"
INITRD="$SMOKE/rootfs.cpio.gz"
RUN="$SMOKE/run"
SERIAL="$RUN/serial.log"
MON="$RUN/monitor.sock"
QEMUPID="$RUN/qemu.pid"
SHOT="$RUN/shot.ppm"
VNC_DISP="127.0.0.1:99"   # :99 => TCP 5999, far from CinuxOS default :0

log() { printf '[smoke] %s\n' "$*" >&2; }
die() { log "FAIL: $*"; cleanup; exit 1; }
need() { command -v "$1" >/dev/null 2>&1 || die "missing tool: $1"; }
cleanup() {
  if [[ -S "$MON" ]]; then
    printf 'quit\n' | socat - UNIX-CONNECT:"$MON" >/dev/null 2>&1 || true
  fi
  if [[ -f "$QEMUPID" ]]; then
    local pid; pid=$(cat "$QEMUPID" 2>/dev/null || true)
    [[ -n "${pid:-}" ]] && kill "$pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT

log "preflight"
need qemu-system-x86_64
need socat
[[ -f "$BZIMAGE" ]] || die "no bzImage at $BZIMAGE (run scripts/build_kernel_x86_64.sh)"
[[ -f "$INITRD"  ]] || die "no initramfs at $INITRD (run scripts/build_initramfs.sh)"

mkdir -p "$RUN"; rm -f "$RUN"/*

log "launch QEMU: self-built kernel + initramfs (vga std / usb-tablet / vnc $VNC_DISP)"
qemu-system-x86_64 \
  -m 512 \
  -kernel "$BZIMAGE" \
  -initrd "$INITRD" \
  -append "console=ttyS0 rdinit=/init video=vesafb:1024x768-32" \
  -vga std \
  -device qemu-xhci,id=xhci \
  -device usb-tablet \
  -display "vnc=$VNC_DISP" \
  -serial "file:$SERIAL" \
  -monitor "unix:$MON,server,nowait" \
  -pidfile "$QEMUPID" \
  -daemonize || die "qemu launch failed"
log "qemu: pid=$(cat "$QEMUPID") vnc=$VNC_DISP (TCP 5999) serial=$SERIAL"

cat >&2 <<EOF

[smoke] Optional: VNC viewer to $VNC_DISP (localhost:5999) for the framebuffer.
[smoke] Guest /init runs fbdev-host automatically; serial streaming to $SERIAL...

EOF

log "wait: GUEST_RUN_DONE on serial (up to 120s for boot + 40s fbdev-host run)"
for _ in $(seq 1 120); do
  grep -q 'GUEST_RUN_DONE' "$SERIAL" 2>/dev/null && break
  sleep 1
done

# screenshot mid-run if we caught RUN_START (diagnostic, not gating)
if grep -q 'GUEST_RUN_START' "$SERIAL" 2>/dev/null && [[ -S "$MON" ]]; then
  log "shot: screendump -> $SHOT"
  printf 'screendump %s\n' "$SHOT" | socat - UNIX-CONNECT:"$MON" >/dev/null 2>&1 || true
fi

log "gate: evaluating serial log"
ok=1
grep -q 'GUEST_FB0_OK' "$SERIAL" 2>/dev/null      || { log "FAIL gate1: no /dev/fb0 (vesafb?)"; ok=0; }
grep -q 'GUEST_TABLET_OK' "$SERIAL" 2>/dev/null   || { log "FAIL gate2: no tablet event node"; ok=0; }
grep -Eq 'fbdev-host: fb [0-9]+x[0-9]+ stride' "$SERIAL" 2>/dev/null \
  || { log "FAIL gate3a: fbdev-host did not open /dev/fb0"; ok=0; }
grep -q 'cannot open' "$SERIAL" 2>/dev/null \
  && { log "FAIL gate3b: fbdev-host reported 'cannot open'"; ok=0; }
if grep -Eq 'Segmentation fault|Bus error|kernel BUG|Oops:' "$SERIAL" 2>/dev/null; then
  log "FAIL gate4: crash signature"; ok=0
fi
echo "[smoke]   fb0:    $(grep 'GUEST_FB0_OK' "$SERIAL" 2>/dev/null | tail -1)" >&2
echo "[smoke]   tablet: $(grep 'GUEST_TABLET_EVENT=' "$SERIAL" 2>/dev/null | tail -1)" >&2
echo "[smoke]   rc:     $(grep 'GUEST_RUN_RC=' "$SERIAL" 2>/dev/null | tail -1)  (124 = timeout, expected)" >&2

if [[ "$ok" -eq 1 ]]; then
  log "PASS -- boot + devices + fbdev-host loop, no crash."
  [[ -f "$SHOT" ]] && log "  screenshot: $SHOT (view / convert to png to eyeball)"
  log "  serial log: $SERIAL"
  exit 0
fi
log "FAIL -- see $SERIAL; $SHOT (if present) for the last frame."
exit 1
