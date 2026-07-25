#!/bin/bash
# @file scripts/run-release.sh
# @brief Cinux Release 一键启动脚本(v1.0.0)
#
# 从 GitHub Release 下载 boot disk + rootfs + 本脚本后,
# `bash run-release.sh [console|desktop]` 一键拉起 QEMU。
# 自动探测同目录的 cinux-v*.img / cinux-v*-{console,desktop}.ext2,
# 或用 CINUX_IMG / CINUX_ROOTFS 环境变量显式指定。
#
# 显示走 VNC :0 —— 用 `vncviewer localhost:0` 连。

set -e

VARIANT="${1:-desktop}"
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

BOOT_IMG="${CINUX_IMG:-$(ls "$DIR"/cinux-v*.img 2>/dev/null | head -1)}"
ROOTFS="${CINUX_ROOTFS:-$(ls "$DIR"/cinux-v*-"$VARIANT".ext2 2>/dev/null | head -1)}"

if [ -z "$BOOT_IMG" ] || [ ! -f "$BOOT_IMG" ]; then
    echo "❌ 找不到 boot disk (cinux-v*.img)" >&2
    echo "   请把它放在 $DIR,或设 CINUX_IMG=<path>" >&2
    exit 1
fi
if [ -z "$ROOTFS" ] || [ ! -f "$ROOTFS" ]; then
    echo "❌ 找不到 rootfs (cinux-v*-$VARIANT.ext2)" >&2
    echo "   variant 可选:console(精简) | desktop(含 gcc/g++)" >&2
    echo "   或设 CINUX_ROOTFS=<path>" >&2
    exit 1
fi

QEMU="${QEMU:-qemu-system-x86_64}"
echo "🚀 Cinux Release 启动"
echo "   Boot disk : $BOOT_IMG"
echo "   Rootfs    : $ROOTFS  (挂 NVMe namespace)"
echo "   SMP       : 2 核"
echo "   显示      : VNC :0  (用 vncviewer localhost:0 连)"
echo "   串口      : stdio  (Ctrl+A X 退出 QEMU)"
echo ""

# 命令复刻 cmake/qemu.cmake 的 `run` target(已验证可 boot):
#   boot disk 挂 -drive index=0;rootfs 挂 NVMe namespace(production 默认路径);
#   e1000 NIC + qemu-xhci USB HID(键鼠)。
exec "$QEMU" \
    -m 1G \
    -smp 2 \
    -serial stdio \
    -no-reboot \
    -cpu max \
    -vnc :0 \
    -usb \
    -drive file="$ROOTFS",format=raw,if=none,id=nvme-disk \
    -device nvme,id=nvme0,serial=nvme0 \
    -device nvme-ns,drive=nvme-disk,nsid=1,bus=nvme0 \
    -device e1000,netdev=net0 -netdev user,id=net0 \
    -device qemu-xhci,id=xhci \
    -device usb-kbd,bus=xhci.0 \
    -device usb-tablet,bus=xhci.0 \
    -drive file="$BOOT_IMG",format=raw,index=0,media=disk
