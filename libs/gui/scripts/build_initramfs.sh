#!/usr/bin/env bash
# scripts/build_initramfs.sh -- pack the P1 initramfs.
# busybox (static) + fbdev-host (static) + /init that runs fbdev-host.
# Output: build/smoke/rootfs.cpio.gz (feed to QEMU -initrd).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUSYBOX_ROOT="$ROOT/build/smoke/busybox-root"
FBDEV_HOST="$ROOT/build/smoke/fbdev-host-static"
INITRAMFS_ROOT="$ROOT/build/smoke/initramfs"
CPIO="$ROOT/build/smoke/rootfs.cpio.gz"
# how long (s) /init lets fbdev-host run before timeout-killing it back to a
# shell. 40s for CI smoke; override FBDEV_TIMEOUT for interactive VNC viewing.
FBDEV_TIMEOUT="${FBDEV_TIMEOUT:-40}"
log() { printf '[initramfs] %s\n' "$*"; }
die() { log "FAIL: $*"; exit 1; }

[[ -d "$BUSYBOX_ROOT/bin" ]] || die "busybox not built (run build_busybox_x86_64.sh)"
[[ -x "$FBDEV_HOST" ]] || die "fbdev-host static not built (run build_fbdev_host_static.sh)"

rm -rf "$INITRAMFS_ROOT"
mkdir -p "$INITRAMFS_ROOT"
cp -a "$BUSYBOX_ROOT/." "$INITRAMFS_ROOT/"
mkdir -p "$INITRAMFS_ROOT/usr/bin" "$INITRAMFS_ROOT/dev" "$INITRAMFS_ROOT/proc" \
         "$INITRAMFS_ROOT/sys" "$INITRAMFS_ROOT/tmp" "$INITRAMFS_ROOT/dev/input"
cp "$FBDEV_HOST" "$INITRAMFS_ROOT/usr/bin/fbdev-host"
chmod +x "$INITRAMFS_ROOT/usr/bin/fbdev-host"

# static device nodes (devtmpfs will also populate, but these guarantee early access)
mknod -m 600 "$INITRAMFS_ROOT/dev/console" c 5 1 2>/dev/null || true
mknod -m 660 "$INITRAMFS_ROOT/dev/fb0"     c 29 0 2>/dev/null || true
mknod -m 666 "$INITRAMFS_ROOT/dev/null"    c 1 3 2>/dev/null || true
mknod -m 660 "$INITRAMFS_ROOT/dev/ttyS0"   c 4 64 2>/dev/null || true

# /init -- kernel runs this (rdinit=/init). Output to ttyS0 for the host gates.
cat > "$INITRAMFS_ROOT/init" <<'INIT_EOF'
#!/bin/sh
# fd1/2 are /dev/console (= ttyS0 via kernel cmdline) -- no exec redirect needed.
# set -x traces every step to serial so we can see exactly where /init is.
set -x
echo "GUEST_INIT_OK"
mount -t proc     none /proc
mount -t sysfs    none /sys
mount -t devtmpfs none /dev 2>/dev/null || true
mount -t tmpfs    none /tmp 2>/dev/null || true

if [ -c /dev/fb0 ]; then
  echo "GUEST_FB0_OK name=$(cat /sys/class/graphics/fb0/name 2>/dev/null || echo '?')"
else
  echo "GUEST_FAIL: no /dev/fb0 (vesafb?)"
fi

# discover usb-tablet from /proc/bus/input/devices (proc is always there; the
# sysfs /sys/class/input glob was empty in this guest). Retry: the tablet
# registers ~3.5s, later than /init starts.
EVT=""
i=0
while [ "$i" -lt 10 ] && [ -z "$EVT" ]; do
  i=$((i + 1))
  EVT=$(grep -A6 "USB Tablet" /proc/bus/input/devices 2>/dev/null | grep -o "event[0-9]*" | head -1)
  [ -z "$EVT" ] && sleep 1
done
echo "GUEST_TABLET_EVENT=${EVT:-none}"
[ -n "$EVT" ] && [ -c "/dev/input/$EVT" ] && echo "GUEST_TABLET_OK" || echo "GUEST_FAIL: tablet node"

echo "GUEST_RUN_START"
timeout 40 /usr/bin/fbdev-host /dev/fb0 "/dev/input/$EVT"
echo "GUEST_RUN_RC=$?"
echo "GUEST_RUN_DONE"
exec /bin/sh
INIT_EOF
# inject the requested fbdev-host run duration (heredoc is single-quoted, so we
# patch the literal `timeout 40` after the fact).
sed -i "s/^timeout 40 /timeout ${FBDEV_TIMEOUT} /" "$INITRAMFS_ROOT/init"
chmod +x "$INITRAMFS_ROOT/init"

log "pack: $CPIO"
( cd "$INITRAMFS_ROOT" && find . | cpio -o -H newc 2>/dev/null | gzip > "$CPIO" )
log "done: $(ls -lh "$CPIO" | awk '{print $5, $9}')"
