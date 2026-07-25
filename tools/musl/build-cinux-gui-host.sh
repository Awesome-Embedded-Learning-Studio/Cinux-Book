#!/usr/bin/env bash
#
# build-cinux-gui-host.sh — compile the Cinux userspace GUI host (F-GUI-USERSPACE b3a).
#
# Links the Cinux-GUI host-neutral core (21 freestanding C++ sources under
# third_party/Cinux-GUI/core) + the Cinux host adapter (user/cinux_gui_host/)
# into a static musl ELF the kernel smoke harness fork+execve's at
# /cinux_gui_host. g++ -ffreestanding -fno-rtti -fno-exceptions: core has ZERO
# libstdc++ deps (audited), so operator-new/delete stubs (crt_stub.cpp) +
# musl libc.a are sufficient -- no libstdc++ linkage.
#
# Usage: build-cinux-gui-host.sh [output-path]
#   default output: build/musl/cinux_gui_host
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
SYSROOT="${MUSL_SYSROOT:-$REPO/build/musl-sysroot}"
GUI_DIR="$REPO/third_party/Cinux-GUI"
OUT="${1:-$REPO/build/musl/cinux_gui_host}"

if [ ! -f "$SYSROOT/lib/libc.a" ]; then
    echo "[build-cinux-gui-host] sysroot missing — run tools/musl/build-musl.sh first" >&2
    exit 1
fi

CB="$(g++ -print-file-name=crtbeginS.o)"
CE="$(g++ -print-file-name=crtendS.o)"
mkdir -p "$(dirname "$OUT")"

# Core source list mirrors third_party/Cinux-GUI/CMakeLists.txt add_library(cinux-gui).
CORE_SRCS=(
    "$GUI_DIR/core/compositor.cpp"
    "$GUI_DIR/core/font.cpp"
    "$GUI_DIR/core/gui_core.cpp"
    "$GUI_DIR/core/paint_list.cpp"
    "$GUI_DIR/core/region.cpp"
    "$GUI_DIR/core/swraster.cpp"
    "$GUI_DIR/core/theme.cpp"
    "$GUI_DIR/core/widget.cpp"
    "$GUI_DIR/core/widget/button.cpp"
    "$GUI_DIR/core/widget/container.cpp"
    "$GUI_DIR/core/widget/label.cpp"
    "$GUI_DIR/core/widget/slider.cpp"
    "$GUI_DIR/core/widget/window.cpp"
    "$GUI_DIR/core/widget/window_manager.cpp"
    "$GUI_DIR/core/widget/desktop_icon.cpp"
    "$GUI_DIR/core/widget/terminal.cpp"
    "$GUI_DIR/core/widget/textbox.cpp"
    "$GUI_DIR/core/widget/checkbox.cpp"
    "$GUI_DIR/core/widget/radio.cpp"
    "$GUI_DIR/core/widget/dropdown.cpp"
    "$GUI_DIR/core/abi_check.cpp"
)

echo "[build-cinux-gui-host] compiling -> $OUT"
g++ -static -nostdlib -no-pie \
    -fno-rtti -fno-exceptions -std=c++17 \
    -I "$GUI_DIR/core" \
    -L "$SYSROOT/lib" \
    "$SYSROOT/lib/Scrt1.o" "$SYSROOT/lib/crti.o" "$CB" \
    "$REPO/user/cinux_gui_host/main.cpp" \
    "$REPO/user/cinux_gui_host/crt_stub.cpp" \
    "${CORE_SRCS[@]}" \
    -lc -lgcc \
    "$CE" "$SYSROOT/lib/crtn.o" \
    -o "$OUT"

echo "[build-cinux-gui-host] artifact:"
file "$OUT"
