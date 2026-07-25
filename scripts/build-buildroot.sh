#!/bin/bash
# Download + configure + build the Cinux buildroot base rootfs.ext2.
# Used by CI (gcc-smoke job) and local repro.  Caches well: skips the download
# when the source tree already exists, and buildroot's own make is incremental
# (setup-cinux's cache-buildroot snapshots <output_dir> across runs).
#
# Usage: build-buildroot.sh <output_dir> [defconfig]
#   output_dir : buildroot src + output/ + dl/ land here (e.g. build/buildroot-ci)
#   defconfig  : rootfs/buildroot/<name> (default cinuxos_base_defconfig)
#
# Output: <output_dir>/output/images/rootfs.ext2  +  <output_dir>/output/target/
set -euo pipefail

OUTPUT_DIR="${1:?usage: $0 <output_dir> [defconfig]}"
DEFCONFIG="${2:-cinuxos_base_defconfig}"
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# buildroot resolves a relative O= relative to its own source tree, not the
# caller's cwd -- a relative OUTPUT_DIR ends up nested inside the buildroot
# src dir (e.g. <output>/buildroot-2026.02.3/<output>/output), so every later
# $OUTPUT_DIR/... lookup (sed on output/.config, images/rootfs.ext2) misses it.
# Force absolute up front so O=, BR_SRC, cp, sed all agree.
case "$OUTPUT_DIR" in
    /*) ;;
    *) OUTPUT_DIR="$REPO_ROOT/$OUTPUT_DIR" ;;
esac

BR_VER="2026.02.3"
BR_TARBALL="buildroot-${BR_VER}.tar.gz"
BR_URL="https://buildroot.org/downloads/${BR_TARBALL}"
BR_SRC="$OUTPUT_DIR/buildroot-${BR_VER}"

mkdir -p "$OUTPUT_DIR"

# Download + extract buildroot source (cache-friendly: keep the tarball so a
# cache-hit run does not re-download).
if [ ! -d "$BR_SRC" ]; then
    echo "[buildroot] downloading buildroot $BR_VER..."
    cache_tarball="$OUTPUT_DIR/$BR_TARBALL"
    if [ ! -f "$cache_tarball" ]; then
        curl -fL "$BR_URL" -o "$cache_tarball"
    fi
    tar xzf "$cache_tarball" -C "$OUTPUT_DIR"
fi

# Configure from the Cinux defconfig.  BR2_DL_DIR isolates the toolchain
# download cache under <output_dir>/dl so it survives across runs.
cp "$REPO_ROOT/rootfs/buildroot/$DEFCONFIG" "$BR_SRC/configs/$DEFCONFIG"
# $DEFCONFIG is already the buildroot defconfig target name (file
# configs/<name>_defconfig, make target <name>_defconfig). Appending
# _defconfig again makes buildroot look for <name>_defconfig_defconfig.
make -C "$BR_SRC" O="$OUTPUT_DIR/output" BR2_DL_DIR="$OUTPUT_DIR/dl" \
     "${DEFCONFIG}"

# The defconfig's BR2_ROOTFS_OVERLAY is relative to the buildroot source tree
# ("../../rootfs/overlay"), which only resolves from the in-tree location used
# during stage 1.  With O=<output_dir> it points at the wrong place, so pin it
# to the absolute repo path here.
sed -i "s|^BR2_ROOTFS_OVERLAY=.*|BR2_ROOTFS_OVERLAY=\"$REPO_ROOT/rootfs/overlay\"|" \
    "$OUTPUT_DIR/output/.config"

# Build.  First run ~5-10 min (downloads Bootlin musl toolchain + builds
# busybox); cached runs skip dl/ and rebuild incrementally.
make -C "$BR_SRC" O="$OUTPUT_DIR/output" BR2_DL_DIR="$OUTPUT_DIR/dl" -j"$(nproc)"

echo "[buildroot] rootfs.ext2 -> $OUTPUT_DIR/output/images/rootfs.ext2"
echo "[buildroot] target/     -> $OUTPUT_DIR/output/target/"
