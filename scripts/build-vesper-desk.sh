#!/usr/bin/env bash
# Builds the GUI station's desk: vesper's fbdev_desktop, statically
# linked against glibc, from the desktop repo's own tree. The build is
# Cinux's own out-of-tree copy under build/third_party/vesper, so the guest
# binary's provenance is this station's build, not whatever the desktop
# repo's own build directory last produced; the tested-against commit
# lives in the station pack. A missing repo fails loudly here rather
# than shipping a stale desk.
set -euo pipefail

out="${1:?usage: build-vesper-desk.sh <output-binary> [build-dir] [source-dir]}"
repo="$(cd "$(dirname "$0")/.." && pwd)"
vesper_root="${3:-${repo}/third_party/vesper}"
build="${2:-${repo}/build/third_party/vesper}"

if [ ! -f "${vesper_root}/CMakeLists.txt" ]; then
    echo "[vesper-desk] no desktop repo at ${vesper_root} (run git submodule update --init --recursive)" >&2
    exit 1
fi

cmake -S "${vesper_root}" -B "${build}" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_COMPILER=g++ \
    -DCMAKE_CXX_FLAGS="-static" \
    -DCMAKE_EXE_LINKER_FLAGS="-static" >/dev/null
cmake --build "${build}" --parallel "${CINUX_ROOTFS_JOBS:-4}" --target fbdev_desktop >/dev/null

mkdir -p "$(dirname "${out}")"
cp "${build}/examples/fbdev_desktop" "${out}"
if ! file "${out}" 2>/dev/null | grep -q "statically linked\|static-pie"; then
    echo "[vesper-desk] binary is NOT static (glibc-static missing?)" >&2
    exit 1
fi
echo "[vesper-desk] built $(ls -lh "${out}" | awk '{print $5}') static desk"
