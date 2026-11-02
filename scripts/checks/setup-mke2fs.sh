#!/usr/bin/env bash
set -euo pipefail

prefix=${1:?usage: setup-mke2fs.sh <installation-prefix>}
prefix=$(realpath -m "$prefix")
work=$(mktemp -d "${TMPDIR:-/tmp}/cinux-e2fsprogs-XXXXXX")
trap 'rm -rf "$work"' EXIT
curl -fL --retry 3 -o "$work/source.tar.xz" \
    https://www.kernel.org/pub/linux/kernel/people/tytso/e2fsprogs/v1.47.2/e2fsprogs-1.47.2.tar.xz
echo "08242e64ca0e8194d9c1caad49762b19209a06318199b63ce74ae4ef2d74e63c  $work/source.tar.xz" | sha256sum -c -
tar -xJf "$work/source.tar.xz" -C "$work"
cd "$work/e2fsprogs-1.47.2"
./configure --prefix="$prefix" --sysconfdir="$prefix/etc" --enable-libarchive --disable-nls
make -j"${CINUX_CHECK_JOBS:-4}"
make install DESTDIR="$work/install"
mkdir -p "$prefix"
cp -a "$work/install$prefix/." "$prefix/"
mkdir "$work/seed"
echo tar-input >"$work/seed/probe"
tar -C "$work/seed" -cf "$work/seed.tar" probe
truncate -s 8M "$work/probe.img"
"$prefix/sbin/mkfs.ext2" -q -F -O none -d "$work/seed.tar" "$work/probe.img"
"$prefix/sbin/debugfs" -R 'cat /probe' "$work/probe.img" 2>/dev/null | grep -qx tar-input
