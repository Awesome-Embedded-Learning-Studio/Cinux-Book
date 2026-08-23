#!/usr/bin/env bash
# scripts/build_kernel_x86_64.sh -- build a minimal x86_64 kernel for the P1 smoke.
#
# Uses x86_64_defconfig (which ships with FB_VESA / INPUT_EVDEV / USB_HID /
# FRAMEBUFFER_CONSOLE all =y) so the guest gets /dev/fb0 (vesafb) + an evdev
# pointer (QEMU usb-tablet). Source defaults to the PenguinLab kernel tree.
#
# Output: build/smoke/linux-build/arch/x86/boot/bzImage

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="${KERNEL_SRC:-/home/charliechen/PenguinLab/third_party/linux}"
OUT="$ROOT/build/smoke/linux-build"
BZIMAGE="$OUT/arch/x86/boot/bzImage"

log() { printf '[kernel] %s\n' "$*"; }
die() { log "FAIL: $*"; exit 1; }

[[ -f "$SRC/Makefile" ]] || die "kernel source not found: $SRC (set KERNEL_SRC)"
mkdir -p "$OUT"
log "source: $SRC"
log "out:    $OUT"

cd "$SRC"

if [[ ! -f "$OUT/.config" ]]; then
  log "config: x86_64_defconfig"
  make O="$OUT" x86_64_defconfig
fi

# x86_64_defconfig does NOT enable FB / FB_VESA / fbcon / USB_XHCI by default
# (server profile). Force them on for P1 (vesafb /dev/fb0 + usb-tablet + fbcon),
# then olddefconfig resolves the rest of the dependency chains.
log "force-enable P1 symbols (FB + vesafb + fbcon + xhci + usbhid + evdev)"
"$SRC/scripts/config" --file "$OUT/.config" \
  --enable CONFIG_FB \
  --enable CONFIG_FB_VESA \
  --enable CONFIG_FRAMEBUFFER_CONSOLE \
  --enable CONFIG_VT \
  --enable CONFIG_VT_CONSOLE \
  --enable CONFIG_USB_SUPPORT \
  --enable CONFIG_USB \
  --enable CONFIG_USB_XHCI \
  --enable CONFIG_USB_XHCI_PCI \
  --enable CONFIG_HID \
  --enable CONFIG_USB_HID \
  --enable CONFIG_INPUT_EVDEV \
  --enable CONFIG_DRM \
  --enable CONFIG_DRM_KMS_HELPER \
  --enable CONFIG_DRM_FBDEV_EMULATION \
  --enable CONFIG_DRM_BOCHS
make O="$OUT" olddefconfig
log "post-olddefconfig P1 symbol check:"
for sym in CONFIG_FB CONFIG_FB_VESA CONFIG_FRAMEBUFFER_CONSOLE CONFIG_USB_XHCI \
           CONFIG_USB_HID CONFIG_INPUT_EVDEV; do
  log "  $sym=$(grep "^${sym}=" "$OUT/.config" | cut -d= -f2- || echo missing)"
done

log "build: bzImage (-j$(nproc)) -- this takes ~15-25 min on a cold tree"
make O="$OUT" -j"$(nproc)" bzImage

[[ -f "$BZIMAGE" ]] || die "bzImage not produced at $BZIMAGE"
log "done: $(ls -lh "$BZIMAGE" | awk '{print $5, $9}')"
