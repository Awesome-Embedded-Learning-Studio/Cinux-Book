#!/usr/bin/env bash
# Builds the glibc-linked dynamic busybox the ecosystem station seats as
# /bin/sh during the rootfs blood swap: same source tree and config as
# the musl build, compiled with the host gcc so the binary carries
# PT_INTERP = /lib64/ld-linux-x86-64.so.2 and DT_NEEDED libc.so.6. The
# output caches at build/busybox-glibc/busybox; remove it to rebuild.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
out="${1:-$(cd "${here}/.." && pwd)/build/busybox-glibc/busybox}"

source_tree="${CINUX_BUSYBOX_SOURCE:-}"
if [ -z "$source_tree" ]; then
    version=1.38.0
    checksum=34f9ea6ff8636f2c9241153b9114eefa9e65674a45318ae1ef95bb5f31c53bb2
    cache="${CINUX_ROOTFS_CACHE:-$(cd "${here}/.." && pwd)/.cache/rootfs}"
    archive="${cache}/busybox-${version}.tar.bz2"
    mkdir -p "$cache"
    if [ ! -f "$archive" ]; then
        curl -fL --retry 3 --output "${archive}.part" \
            "https://busybox.net/downloads/busybox-${version}.tar.bz2"
        mv "${archive}.part" "$archive"
    fi
    printf '%s  %s\n' "$checksum" "$archive" | sha256sum -c -
    if [ ! -d "${cache}/busybox-${version}" ]; then
        tar -xjf "$archive" -C "$cache"
    fi
    source_tree="${cache}/busybox-${version}"
fi
if [ ! -d "$source_tree" ]; then
    echo "[build-busybox-glibc] source tree missing: ${source_tree}" >&2
    exit 1
fi

gcc="$(command -v gcc || true)"
if [ -z "$gcc" ]; then
    echo "[build-busybox-glibc] gcc not found on PATH" >&2
    exit 1
fi

script_abs="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")"
source_fingerprint="$(python3 - "$source_tree" <<'PY'
import hashlib
import pathlib
import sys
root = pathlib.Path(sys.argv[1])
digest = hashlib.sha256()
for path in sorted(root.rglob('*')):
    if path.is_file() and (path.suffix in ('.c', '.h', '.S', '.in') or
                           path.name in ('Makefile', 'Kbuild', 'Config.in')):
        digest.update(str(path.relative_to(root)).encode())
        digest.update(path.read_bytes())
print(digest.hexdigest())
PY
)"
fingerprint="$({ sha256sum "${here}/../user/rootfs/busybox/busybox.config" "$script_abs";
                 printf '%s\n' "$source_fingerprint";
                 sha256sum "$(gcc -print-file-name=libc.so.6)";
                 gcc --version; gcc -dumpmachine; } | sha256sum | cut -d ' ' -f 1)"
if [ -f "$out" ] && [ -f "${out}.inputs" ] && \
   [ "$(cat "${out}.inputs")" = "$fingerprint" ]; then
    echo "[build-busybox-glibc] cache hit, keeping ${out}"
    exit 0
fi

work="$(mktemp -d /tmp/cinux-busybox-glibc.XXXXXX)"
trap 'rm -rf "$work"' EXIT
cp -a "$source_tree"/. "$work"/
make -C "$work" clean >/dev/null
cp "${here}/../user/rootfs/busybox/busybox.config" "$work/.config"
python3 - "$work/.config" <<'EOF'
import sys
path = sys.argv[1]
with open(path) as handle:
    lines = handle.readlines()
with open(path, "w") as handle:
    for line in lines:
        if line.startswith("CONFIG_STATIC=y"):
            handle.write("# CONFIG_STATIC is not set\n")
        elif line.startswith("CONFIG_EXTRA_CFLAGS="):
            handle.write('CONFIG_EXTRA_CFLAGS="-fno-pie"\n')
        elif line.startswith("CONFIG_EXTRA_LDFLAGS="):
            handle.write('CONFIG_EXTRA_LDFLAGS="-no-pie -Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2 -Wl,-rpath=/lib"\n')
        else:
            handle.write(line)
EOF
make -C "$work" CC="$gcc" HOSTCC=cc -j"${CINUX_ROOTFS_JOBS:-4}" >/dev/null
mkdir -p "$(dirname "$out")"
cp "$work/busybox" "$out"
printf '%s\n' "$fingerprint" > "${out}.inputs"
file "$out"
readelf -l "$out" | grep -m1 interpreter || true
