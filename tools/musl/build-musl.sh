#!/usr/bin/env bash
#
# build-musl.sh — build a self-contained musl sysroot for CinuxOS user programs.
#
# Produces $MUSL_SYSROOT/{lib/libc.a, crt1.o, Scrt1.o, rcrt1.o, crti.o, crtn.o,
# include/...}.  CinuxOS targets the Linux x86_64 ABI, so we build musl natively
# with the host GCC (no cross prefix).  The resulting libc.a is what static
# CinuxOS user programs (musl hello world, later CFBox) link against.
#
# Idempotent: skips work if $MUSL_SYSROOT/lib/libc.a already exists.
#
# Gotchas baked in (see tools/musl/README.md):
#   * Do NOT pass --target to configure: musl prefixes AR/RANLIB with it
#     (x86_64-ar), which the host lacks.  Pass CC=gcc AR=ar RANLIB=ranlib.
#   * musl-gcc wrapper is unusable on GCC>=14: the host specs inject
#     -latomic_asneeded, which musl's specs don't suppress.  We link user
#     programs manually instead (build-hello.sh).
set -euo pipefail

MUSL_VER=1.2.5
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
SYSROOT="${MUSL_SYSROOT:-$REPO/build/musl-sysroot}"
WORK="$REPO/build/musl"
SRC="$WORK/musl-$MUSL_VER"
TARBALL="$WORK/musl-$MUSL_VER.tar.gz"

if [ -f "$SYSROOT/lib/libc.a" ]; then
    echo "[build-musl] sysroot already present at $SYSROOT (rm -rf to rebuild)"
    exit 0
fi

mkdir -p "$WORK" "$SYSROOT"

# 1. Fetch the official tarball (cgit clone redirects; tarball is stable).
if [ ! -f "$TARBALL" ]; then
    echo "[build-musl] downloading musl-$MUSL_VER..."
    curl -fsSL "https://musl.libc.org/releases/musl-$MUSL_VER.tar.gz" -o "$TARBALL"
fi

# 2. Extract.
if [ ! -d "$SRC" ]; then
    echo "[build-musl] extracting..."
    tar -C "$WORK" -xzf "$TARBALL"
fi

# 3. Configure (native x86_64; explicit CC/AR/RANLIB, NO --target).
cd "$SRC"
if [ ! -f config.mak ]; then
    echo "[build-musl] configuring..."
    CC=gcc AR=ar RANLIB=ranlib ./configure --prefix="$SYSROOT"
fi

# 4. Build + install.
echo "[build-musl] building with $(nproc) jobs..."
make -j"$(nproc)"
make install

echo "[build-musl] done. sysroot at $SYSROOT"
ls -la "$SYSROOT/lib/libc.a" "$SYSROOT/lib/crt1.o" "$SYSROOT/lib/Scrt1.o"
