#!/bin/bash
# Assemble the gcc-profile rootfs.ext2: buildroot base target + GCC toolchain
# closure (extract.sh) + overlay, packed with mkfs.ext2 -d.  The result ships a
# native gcc driver so `gcc /hello.c` runs on Cinux as a default-PIE binary.
#
# Usage: assemble_gcc_rootfs.sh <output_img> [buildroot_target] [gcc_root]
#   buildroot_target : buildroot output/target dir
#                      (default $REPO/build/buildroot/output/target)
#   gcc_root         : extract.sh output dir (default $REPO/build/gcc-root;
#                      recreated on each assemble to avoid stale host closures)
#
# This is the gcc-profile counterpart to the handcrafted create_ext2_disk.sh:
# buildroot builds the base musl/busybox target out-of-tree, and this script
# merges in the host GCC closure (glibc-dynamic) plus the Cinux overlay.  The
# two dynamic loaders coexist: /lib64/ld-linux-x86-64.so.2 (gcc/cc1/as/ld) and
# /lib/ld-musl-x86_64.so.1 (busybox).
set -euo pipefail

OUTPUT="${1:?usage: $0 <output_img> [buildroot_target] [gcc_root]}"
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# BR_TARGET / GCC_ROOT may be overridden via the environment too -- CI uses a
# buildroot output dir different from the stage-1 default (build/buildroot-ci).
BR_TARGET="${2:-${BR_TARGET:-$REPO_ROOT/build/buildroot/output/target}}"
GCC_ROOT="${3:-${GCC_ROOT:-$REPO_ROOT/build/gcc-root}}"

if [ ! -d "$BR_TARGET" ]; then
    echo "[assemble] error: buildroot target dir not found: $BR_TARGET" >&2
    echo "[assemble]        build the base rootfs first (buildroot make)." >&2
    exit 1
fi

# Stage the GCC closure every time.  It is quick, and stale closures are easy to
# hit when iterating locally or under act with a reused build directory.
echo "[assemble] staging GCC closure via extract.sh..."
"$REPO_ROOT/tools/gcc-toolchain/extract.sh" "$GCC_ROOT" >/dev/null

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
cp -a "$BR_TARGET" "$WORK/target"

# buildroot's musl layout symlinks /lib64 -> /lib and /usr/lib64 -> /usr/lib;
# the GCC closure ships both as real dirs (glibc's ld-linux-x86-64.so.2 lands in
# each).  Drop the symlinks so the real dirs merge in -- both loaders coexist
# (see header comment).  Newer buildroot also adds /usr/lib64 -> /usr/lib, which
# the C++ closure (cc1plus/libstdc++) started exercising.
rm -f "$WORK/target/lib64" "$WORK/target/usr/lib64"
cp -a "$GCC_ROOT/." "$WORK/target/"

# The latest overlay (inittab + usability script) wins over the merged tree.
cp "$REPO_ROOT/rootfs/overlay/etc/inittab" "$WORK/target/etc/inittab"
cp "$REPO_ROOT/rootfs/overlay/etc/cinux-usability-test.sh" "$WORK/target/etc/cinux-usability-test.sh"

# F-GUI-USERSPACE b3b: stage the userspace GUI host ELF (static musl, built by
# tools/musl/build-cinux-gui-host.sh). Self-contained -- no libstdc++/glibc deps
# -- so it sits beside the gcc closure with no extra staging. launch_userspace
# fork+execve's /cinux_gui_host at boot; absent -> ENOENT -> no desktop.
GUI_HOST="${CINUX_GUI_HOST_ELF:-$REPO_ROOT/build/musl/cinux_gui_host}"
if [ -f "$GUI_HOST" ]; then
    cp -p "$GUI_HOST" "$WORK/target/cinux_gui_host"
    echo "[assemble] + /cinux_gui_host (userspace GUI host, b3b)"
else
    echo "[assemble] WARNING: $GUI_HOST missing -- run tools/musl/build-cinux-gui-host.sh first" >&2
fi

# Defense (2026-07-04 regression): force the link-time crt objects to the gcc
# toolchain's authoritative relocatable versions.  An executable crt1.o left in
# the merged tree makes ld reject the link ("cannot use executable file 'crt1.o'
# as input") with a cascade of spurious multiple-definition errors.  extract.sh
# already stages these, but the buildroot-target merge has intermittently left
# an executable crt1.o behind (root cause never pinned -- staging verifies
# relocatable yet the packed rootfs came out executable).  Overwrite from the
# gcc -print-file-name source + sanity-check so a future regression fails the
# build loudly instead of shipping a broken rootfs.
gccbin="${GCC_BIN:-gcc}"
for f in crt1.o Scrt1.o crti.o crtn.o; do
    src="$("$gccbin" -print-file-name="$f")"
    if [ -n "$src" ] && [ "$src" != "$f" ] && [ -e "$src" ]; then
        cp -aL "$src" "$WORK/target/usr/lib/$f"
    fi
done
for c in "$WORK/target/usr/lib/crt1.o" "$WORK/target/lib/crt1.o"; do
    [ -e "$c" ] || continue
    case "$(file -b "$c")" in
        *relocatable*) ;;
        *)
            echo "[assemble] ERROR: $c is not relocatable (would break ld):" >&2
            file "$c" >&2
            exit 1
            ;;
    esac
done

# 192 MB / 16384 inodes holds base (~5 MB) + the C/C++ GCC closure (~127 MB:
# cc1 + cc1plus + libstdc++ + the C and C++ header trees).  Plain ext2 (-O none,
# no extents): the ext2 driver only *reads* ext4 extents (F6-M5); extent writes
# are a follow-up, so a writable rootfs must stay ext2 for now.  block_size=1024
# matches the driver's expectation.
dd if=/dev/zero of="$OUTPUT" bs=1M count=192 status=none
mkfs.ext2 -q -b 1024 -O none -N 16384 -d "$WORK/target" "$OUTPUT"

echo "[assemble] gcc rootfs -> $OUTPUT ($(du -h "$OUTPUT" | cut -f1))"
