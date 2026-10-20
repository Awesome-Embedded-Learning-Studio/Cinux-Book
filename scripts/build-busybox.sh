#!/usr/bin/env bash
# Builds the static busybox the teaching volume seats as /bin/sh, from the
# source tree kept at ~/.local/src/busybox-1.38.0 (config included). The
# output caches at build/busybox/busybox; remove it to force a rebuild.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
out="${1:-$(cd "${here}/.." && pwd)/build/busybox/busybox}"

source_tree="${HOME}/.local/src/busybox-1.38.0"
if [ ! -d "$source_tree" ]; then
    echo "[build-busybox] source tree missing: ${source_tree}" >&2
    exit 1
fi

musl_gcc="$(command -v musl-gcc || true)"
if [ -z "$musl_gcc" ] && [ -x "${HOME}/.local/musl/bin/musl-gcc" ]; then
    musl_gcc="${HOME}/.local/musl/bin/musl-gcc"
fi
if [ -z "$musl_gcc" ]; then
    echo "[build-busybox] musl-gcc not found on PATH or ~/.local/musl" >&2
    exit 1
fi

work="$(mktemp -d /tmp/cinux-busybox-build.XXXXXX)"
trap 'rm -rf "$work"' EXIT
cp -a "$source_tree"/. "$work"/
make -C "$work" CC="$musl_gcc" HOSTCC=cc -j"$(nproc)" >/dev/null
mkdir -p "$(dirname "$out")"
cp "$work/busybox" "$out"
file "$out"
