#!/usr/bin/env bash
# Builds the glibc-linked hello the ecosystem station fires: a non-PIE
# ET_EXEC carrying PT_INTERP = /lib64/ld-linux-x86-64.so.2, linked
# against the host's glibc. The baked interpreter path is the guest's
# path, so -Wl,-dynamic-linker overrides the host toolchain's own, and
# libc.so.6 rides along on the volume at /lib/libc.so.6, a directory the
# loader searches by default when no cache file exists.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
out="${1:-${here}/hello-glibc}"

gcc="$(command -v gcc || true)"
if [ -z "$gcc" ]; then
    echo "[build-hello-glibc] gcc not found on PATH" >&2
    exit 1
fi

out_dir="$(cd "$(dirname "$out")" 2>/dev/null && pwd)" || out_dir=""
if [ -z "$out_dir" ]; then
    mkdir -p "$(dirname "$out")"
    out_dir="$(cd "$(dirname "$out")" && pwd)"
fi
out="${out_dir}/$(basename "$out")"

src="${out}.c"
cat > "$src" <<'EOF'
#include <stdio.h>

int main() {
    printf("Hello from dynamic glibc on Cinux!\n");
    return 0;
}
EOF

"$gcc" -no-pie -Os -Wl,--build-id=none \
    -Wl,-dynamic-linker,/lib64/ld-linux-x86-64.so.2 \
    -Wl,-rpath,/lib \
    "$src" -o "$out"

file "$out"
readelf -hl "$out" | sed -n '1,30p'
