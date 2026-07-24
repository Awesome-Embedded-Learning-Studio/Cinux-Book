#!/usr/bin/env bash
#
# build-musl.sh — build a self-contained musl sysroot for Cinux user programs.
#
# Produces $MUSL_SYSROOT/{lib/libc.a, crt1.o, Scrt1.o, rcrt1.o, crti.o, crtn.o,
# include/...}.  Cinux targets the Linux x86_64 ABI, so we build musl natively
# with the host GCC (no cross prefix).  The resulting libc.a is what static
# Cinux user programs (musl hello world, later CFBox) link against.
#
# Idempotent: skips work if $MUSL_SYSROOT/lib/libc.a already exists.
#
# Gotchas baked in (see tools/musl/README.md):
#   * Do NOT pass --target to configure: musl prefixes AR/RANLIB with it
#     (x86_64-ar), which the host lacks.  Pass CC=gcc AR=ar RANLIB=ranlib.
#   * GCC>=14 host specs inject -latomic_asneeded on static links; the musl
#     sysroot has no libatomic_asneeded.a.  musl doesn't need libatomic (uses
#     inline __atomic), so the "Patch musl-gcc" step adds -fno-link-libatomic to
#     the wrapper -- musl-gcc is then usable on GCC16 (busybox links clean).
set -euo pipefail

MUSL_VER=1.2.6
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
SYSROOT="${MUSL_SYSROOT:-$REPO/build/musl-sysroot}"
WORK="$REPO/build/musl"
SRC="$WORK/musl-$MUSL_VER"
TARBALL="$WORK/musl-$MUSL_VER.tar.gz"

install_linux_uapi_headers() {
    if [ -f "$SYSROOT/include/linux/kd.h" ] &&
        [ -f "$SYSROOT/include/asm/ioctls.h" ] &&
        [ -f "$SYSROOT/include/mtd/mtd-user.h" ] &&
        [ -f "$SYSROOT/include/scsi/sg.h" ]; then
        return
    fi

    if [ ! -d /usr/include/linux ] || [ ! -d /usr/include/asm-generic ]; then
        echo "[build-musl] missing host Linux UAPI headers; install linux-libc-dev" >&2
        exit 1
    fi

    mkdir -p "$SYSROOT/include"
    for dir in linux asm-generic mtd scsi sound video drm rdma; do
        if [ -d "/usr/include/$dir" ]; then
            cp -a "/usr/include/$dir" "$SYSROOT/include/"
        fi
    done

    if [ -d /usr/include/asm ]; then
        cp -a /usr/include/asm "$SYSROOT/include/"
    elif [ -d /usr/include/x86_64-linux-gnu/asm ]; then
        cp -a /usr/include/x86_64-linux-gnu/asm "$SYSROOT/include/"
    else
        echo "[build-musl] missing host asm UAPI headers; install linux-libc-dev" >&2
        exit 1
    fi

    echo "[build-musl] installed Linux UAPI headers into sysroot"
}

if [ -f "$SYSROOT/lib/libc.a" ]; then
    install_linux_uapi_headers
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
    # musl default -O2. The earlier -O0 workaround is no longer needed: the real
    # root cause was Cinux syscall/ISR clobbering Linux-ABI-preserved user
    # state. Fixed by restoring caller-saved arg registers (a90cc4a) AND
    # saving/restoring user SIMD/FPU via FXSAVE (57e2664) -- musl -O2 uses XMM,
    # and the kernel's own C/C++ code was corrupting user SIMD -> heap damage
    # -> mallocng a_crash.
    CC=gcc AR=ar RANLIB=ranlib ./configure --prefix="$SYSROOT" CFLAGS="-O2"
fi

# 4. Build + install.
echo "[build-musl] building with $(nproc) jobs..."
make -j"$(nproc)"
make install
install_linux_uapi_headers

# 5. Patch musl-gcc: GCC>=14 host specs inject -latomic_asneeded on static
#    links, but the musl sysroot has no libatomic_asneeded.a (and musl doesn't
#    need it -- inline __atomic).  -fno-link-libatomic suppresses it so musl-gcc
#    works on GCC16 for static user programs (busybox, CFBox, ...).
sed -i 's|-specs|-fno-link-libatomic -specs|' "$SYSROOT/bin/musl-gcc"
echo "[build-musl] patched musl-gcc with -fno-link-libatomic (GCC16 fix)"

echo "[build-musl] done. sysroot at $SYSROOT"
ls -la "$SYSROOT/lib/libc.a" "$SYSROOT/lib/crt1.o" "$SYSROOT/lib/Scrt1.o"
