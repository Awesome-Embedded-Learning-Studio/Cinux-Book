#!/usr/bin/env bash
# Builds the dynamic hello the interpreter station fires: a non-PIE ET_EXEC
# carrying PT_INTERP = /lib/ld-musl-x86_64.so.1, linked against the musl
# toolchain's libc.so. The baked interpreter path is the guest's path, not
# the host toolchain's, so -Wl,-dynamic-linker overrides the wrapper's own.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
out="${1:-${here}/hello-dyn}"

musl_gcc="$(command -v musl-gcc || true)"
if [ -z "$musl_gcc" ] && [ -x "${HOME}/.local/musl/bin/musl-gcc" ]; then
    musl_gcc="${HOME}/.local/musl/bin/musl-gcc"
fi
if [ -z "$musl_gcc" ]; then
    echo "[build-hello-dyn] musl-gcc not found on PATH or ~/.local/musl" >&2
    exit 1
fi

src="${out}.c"
cat > "$src" <<'EOF'
#include <stdio.h>

int main() {
    printf("Hello from dynamic musl on Cinux!\n");
    return 0;
}
EOF

"$musl_gcc" -no-pie -Os -Wl,--build-id=none \
    -Wl,-dynamic-linker,/lib/ld-musl-x86_64.so.1 \
    "$src" -o "$out"

file "$out"
readelf -hl "$out" | sed -n '1,30p'
