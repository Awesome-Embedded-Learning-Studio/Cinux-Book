#!/bin/bash
# Stage the GCC toolchain subset for the B4-a as+ld smoke into a directory tree.
#
# Usage: extract.sh [output_dir]   (default build/gcc-root)
#       GCC_BIN=<gcc-binary> extract.sh ...   (default: gcc; CI pins gcc-13)
#
# NOT a self-built toolchain (user decision 2026-07-02: don't compile GCC). This
# copies the host's glibc-dynamic as/ld + cc1/cc1plus + gcc/g++ drivers + their
# runtime .so closure + crt/libgcc/libstdc++ + the C/C++ header closures, laid
# out exactly as GCC's built-in specs expect (hardcoded /usr/lib,
# /usr/lib/gcc/<triple>/<ver>, /lib64 paths).
#
# GCC_BIN selects which host GCC to mirror.  The whole closure -- driver, cc1,
# collect2, libgcc, crt objects, liblto_plugin -- MUST come from one GCC version,
# or cc1 and the driver disagree at runtime.  Default `gcc`; CI sets GCC_BIN=gcc-13
# so the version is explicit and does not drift when ubuntu-latest's default `gcc`
# rolls.  cc1 is located via `gcc -print-prog-name=cc1` (GCC's own spec-driven
# answer), NOT a hardcoded path.
#
# The tree feeds scripts/create_ext2_disk.sh via its GCC_ROOT arg; mkfs.ext2 -d
# merges it (cp -a preserves SONAME symlinks like libbfd.so -> libbfd-2.46.0.so).

set -e

ROOT="${1:-build/gcc-root}"
GCC_BIN="${GCC_BIN:-gcc}"
if ! command -v "$GCC_BIN" >/dev/null 2>&1; then
    echo "[extract] ERROR: GCC_BIN='$GCC_BIN' not found on PATH" >&2
    echo "[extract]        Set GCC_BIN to an installed gcc (e.g. gcc-13), or apt-get install it." >&2
    exit 1
fi
# -print-prog-name=cc1 returns <installdir>/cc1 even when the file is absent (it
# is a spec-driven path, not a stat), so the -x test below is the real check.
CC1="$("$GCC_BIN" -print-prog-name=cc1)"
GCC_PROG_DIR="$(dirname "$CC1")"  # e.g. /usr/libexec/gcc/<triple>/<ver>
GCC_LIBGCC="$("$GCC_BIN" -print-libgcc-file-name)"
GCC_LIB_DIR="$(dirname "$GCC_LIBGCC")"  # e.g. /usr/lib/gcc/<triple>/<ver>
GCC_DRIVER="$(command -v "$GCC_BIN")"

rm -rf "$ROOT"
mkdir -p "$ROOT/usr/bin" "$ROOT/usr/lib" "$ROOT/lib64" "$ROOT$GCC_PROG_DIR" "$ROOT$GCC_LIB_DIR"

copy_abs() {
    local src="$1"
    [ -e "$src" ] || return 0
    mkdir -p "$ROOT$(dirname "$src")"
    cp -aL "$src" "$ROOT$src"
}

copy_gcc_file() {
    local name="$1"
    local src
    src="$("$GCC_BIN" -print-file-name="$name")"
    [ -n "$src" ] || return 0
    [ "$src" != "$name" ] || return 0
    [ -e "$src" ] || return 0
    copy_abs "$src"
}

# --- binaries: as/ld only (cc1 + gcc driver land with B4-b) ---
# Ubuntu's gcc/as/ld entries can be versioned symlinks.  Install real ELF files
# into the rootfs so execve does not chase a dangling host-only symlink.
cp -aL /usr/bin/as "$ROOT/usr/bin/as"
cp -aL /usr/bin/ld "$ROOT/usr/bin/ld"

# --- dynamic interpreter at the exact PT_INTERP path ---
# Resolve the real file (/lib64 is a symlink on Arch); install it as a regular
# file at /lib64/ld-linux-x86-64.so.2 -- the path every glibc-dynamic ELF asks for.
mkdir -p "$ROOT/lib64"
cp -aL /lib64/ld-linux-x86-64.so.2 "$ROOT/lib64/ld-linux-x86-64.so.2"

# Copy a .so, preserving its versioned name AND creating the SONAME symlink
# (libfoo.so.N -> libfoo.so.N.M.P) so ldso resolves DT_NEEDED.
cp_lib() {
    local src="$1"
    [ -f "$src" ] || return 0
    copy_abs "$src"
    cp -aL "$src" "$ROOT/usr/lib/"
    local base soname
    base=$(basename "$src")
    soname=$(readelf -d "$src" 2>/dev/null | sed -n 's/.*SONAME.*\[\(.*\)\].*/\1/p' | head -1)
    if [ -n "$soname" ] && [ "$soname" != "$base" ]; then
        ln -sf "$base" "$ROOT/usr/lib/$soname"
    fi
}

# --- runtime .so closure of as/ld via ldd (libc/libm/libbfd/libctf/libjansson/
#     libz/libzstd/libsframe). ldd prints `lib => /path (addr)`; field 3 is the
#     path. ldso itself prints without `=>` and is skipped (handled above). ---
ldd /usr/bin/as /usr/bin/ld 2>/dev/null | grep '=> /' | awk '{print $3}' | sort -u | while read -r lib; do
    cp_lib "$lib"
done
# Arch GCC's default link specs pass -lgcc_s_asneeded and
# -latomic_asneeded through collect2/ld.  The plain .so targets live in
# /usr/lib (glibc/gcc shared), but the *_asneeded linker scripts live in GCC's
# private libdir ($prefix/lib/gcc/<tuple>/<ver>/), NOT /usr/lib -- a hardcoded
# /usr/lib path silently fails (cp_lib || true) and ld later can't find them
# ("cannot find -lgcc_s_asneeded").  Resolve via gcc -print-file-name.
for f in /usr/lib/libgcc_s.so /usr/lib/libgcc_s.so.1 \
         /usr/lib/libatomic.so /usr/lib/libatomic.so.1 /usr/lib/libatomic.so.1.2.0; do
    cp_lib "$f"
done
cp -a /usr/lib/libatomic.a "$ROOT/usr/lib/" 2>/dev/null || true
for f in libgcc_s_asneeded.so libatomic_asneeded.so libatomic_asneeded.a; do
    copy_gcc_file "$f"
done

# --- B4-C1: cc1 (GCC C front end, ~47 MB) + its .so closure.  cc1 pulls
#     libisl/libmpc/libmpfr/libgmp/libm on top of as/ld's libz/libzstd/libc.
#     `cc1 --version` needs NO headers, so this stages the binary + deps only;
#     /usr/include (the C headers, ~250 MB) lands with B4-C2 (actual compile). ---
# Debian/Ubuntu split cc1 into the cpp-N package (not gcc-N); build-essential
# does not depend on cpp, so a fresh CI runner has the gcc driver + collect2 but
# no cc1.  Fail up front with a clear message instead of a vague cp error below.
if [ ! -x "$CC1" ]; then
    echo "[extract] ERROR: cc1 not found at $CC1 (GCC_BIN=$GCC_BIN)" >&2
    echo "[extract]        On Debian/Ubuntu: apt-get install cpp-N (cc1 ships in cpp-N, not gcc-N)." >&2
    exit 1
fi
copy_abs "$CC1"
ldd "$CC1" 2>/dev/null | grep '=> /' | awk '{print $3}' | sort -u | while read -r lib; do
    cp_lib "$lib"
done

# --- F-USABILITY stage 3: gcc driver + collect2 (single-command `gcc hello.c`
#     path).  The driver (~1.7 MB) forks cc1/as/ld and pipes their stderr; its
#     .so closure is just libc + ldso (already staged via as/ld, so the ldd
#     cp_lib below is a harmless no-op).  collect2 runs at link time to wire
#     .init_array constructors.  liblto_plugin.so is still needed because GCC's
#     default link path passes -fuse-linker-plugin even for this tiny C smoke.
#     cc1plus + the g++ driver land in stage 4 (C++ smoke below); lto1/
#     lto-wrapper still stay out (no -flto in the smoke). ---
# The driver installs at the fixed PT_INTERP-independent name /usr/bin/gcc on the
# rootfs (what Cinux users invoke), but is sourced from $GCC_BIN so it matches
# cc1/collect2/libgcc -- one version end to end.
cp -aL "$GCC_DRIVER" "$ROOT/usr/bin/gcc"
COLLECT2="$("$GCC_BIN" -print-prog-name=collect2)"
copy_abs "$COLLECT2"
copy_gcc_file liblto_plugin.so
ldd "$GCC_DRIVER" "$COLLECT2" 2>/dev/null | grep '=> /' | awk '{print $3}' | sort -u | while read -r lib; do
    cp_lib "$lib"
done

# --- link-time crt + libc + libgcc (ld needs these when linking hello.o -> hello) ---
for f in crt1.o Scrt1.o crti.o crtn.o; do
    copy_gcc_file "$f"
    cp -a "/usr/lib/$f" "$ROOT/usr/lib/" 2>/dev/null || true
done
for f in libc.so libc_nonshared.a libc.a; do
    copy_gcc_file "$f"
    cp -a "/usr/lib/$f" "$ROOT/usr/lib/" 2>/dev/null || true
done
for f in crtbegin.o crtbeginS.o crtbeginT.o crtend.o crtendS.o crtfastmath.o; do
    copy_gcc_file "$f"
done
copy_abs "$GCC_LIBGCC"          # libgcc.a
copy_gcc_file libgcc_eh.a
copy_gcc_file libgcc_s.so
copy_gcc_file libgcc_s.so.1

# --- hello.s: host-precompiled as input for the B4-a as+ld smoke, plus the
#     hello.c source for B4-b cc1. ---
cat > /tmp/cinux_hello.c << 'EOF'
#include <stdio.h>
int main(void) {
    printf("Hello from GCC!\n");
    return 0;
}
EOF
# gcc defaults to PIE on modern distros (--enable-default-pie); Cinux now loads
# PIE main (ET_DYN + ELF-base ASLR, kernel PIE batch 1), so emit PIE assembly
# (RIP-relative, links with Scrt1.o + crtbeginS.o under ld -pie) by leaving the
# default alone. Forcing -fno-pie would yield the legacy ET_EXEC path.
"$GCC_BIN" -S -o "$ROOT/hello.s" /tmp/cinux_hello.c
cp /tmp/cinux_hello.c "$ROOT/hello.c"
rm -f /tmp/cinux_hello.c

# --- B4-C2: hello.c's #include closure (computed live via gcc -H) so cc1 can
#     actually compile hello.c on Cinux.  The closure is tiny (~25 files,
#     ~200 KB); stage exactly those at their absolute paths instead of all
#     ~249 MB of /usr/include.  cc1's built-in include search (/usr/include +
#     GCC's private include dir) then resolves <stdio.h> et al.  [ -f ] filters the
#     "Multiple include guards ..." non-path line gcc -H appends. ---
"$GCC_BIN" -H -fsyntax-only "$ROOT/hello.c" 2>&1 >/dev/null \
    | sed -E 's/^[. ]+//' | grep -v '^$' | sort -u | while read -r h; do
    [ -f "$h" ] || continue
    install -Dm0644 "$h" "$ROOT$h"
done
# GCC's cc1 implicitly pre-includes <stdc-predef.h> before the C source (and
# glibc features.h:518 includes it explicitly).  gcc -H does NOT list this
# implicit pre-include, so the closure above misses it; without it cc1 fails
# at features.h:518 "fatal error: stdc-predef.h: No such file or directory".
[ -f /usr/include/stdc-predef.h ] && install -Dm0644 /usr/include/stdc-predef.h "$ROOT/usr/include/stdc-predef.h"

# --- F-USABILITY stage 4 (C++): cc1plus + g++ driver + libstdc++ + the C++
#     header closure so `g++ /hello.cpp` runs on Cinux as a default-PIE binary.
#     Mirrors the C path: stage the binary + .so closure + headers computed
#     live via g++ -H.  libstdc++.so carries the EH runtime (__cxa_* / STL);
#     libgcc_s.so (staged above) carries the DWARF unwinder (_Unwind_*).  No
#     <thread>/pthread here -- that needs gettid/set_robust_list and is a
#     separate batch. ---
# GXX_BIN mirrors GCC_BIN for the version-matched g++ driver (gcc<->g++,
# gcc-13<->g++-13).  Debian/Ubuntu: g++-N is its own package; fail up front
# (like cc1) instead of a vague cp error below.
GXX_BIN="${GXX_BIN:-${GCC_BIN/gcc/g++}}"
if ! command -v "$GXX_BIN" >/dev/null 2>&1; then
    echo "[extract] ERROR: GXX_BIN='$GXX_BIN' not found on PATH" >&2
    echo "[extract]        apt-get install the matching g++ (e.g. g++-13)." >&2
    exit 1
fi
GXX_DRIVER="$(command -v "$GXX_BIN")"

# cc1plus (GCC C++ front end).  Resolved via g++'s own spec, never hardcoded.
CC1PLUS="$("$GXX_BIN" -print-prog-name=cc1plus)"
if [ ! -x "$CC1PLUS" ]; then
    echo "[extract] ERROR: cc1plus not found at $CC1PLUS (GXX_BIN=$GXX_BIN)" >&2
    echo "[extract]        On Debian/Ubuntu ensure g++-N is installed." >&2
    exit 1
fi
copy_abs "$CC1PLUS"
ldd "$CC1PLUS" 2>/dev/null | grep '=> /' | awk '{print $3}' | sort -u | while read -r lib; do
    cp_lib "$lib"
done

# g++ driver at /usr/bin/g++ (what Cinux users invoke).  Sourced from GXX_BIN
# so it matches cc1plus/collect2/libstdc++ -- one GCC version end to end.
cp -aL "$GXX_DRIVER" "$ROOT/usr/bin/g++"

# libstdc++ runtime + EH: real .so.6.0.X + SONAME (.so.6, for ldso DT_NEEDED) +
# dev symlink (.so, for ld -lstdc++ at g++ link time).  cp_lib installs the real
# file and builds the SONAME symlink; the dev symlink we add explicitly.
LIBSTDCXX_REAL="$(readlink -f "$("$GXX_BIN" -print-file-name=libstdc++.so.6)")"
if [ -n "$LIBSTDCXX_REAL" ] && [ -f "$LIBSTDCXX_REAL" ]; then
    cp_lib "$LIBSTDCXX_REAL"
    ln -sf "$(basename "$LIBSTDCXX_REAL")" "$ROOT/usr/lib/libstdc++.so"
    ln -sf "$(basename "$LIBSTDCXX_REAL")" "$ROOT/usr/lib/libstdc++.so.6"
fi

# libm (glibc): g++ links -lm (libstdc++ pulls libm).  Stage host glibc
# libm.so.6 + a dev symlink.  NOTE: host glibc's libm.so.6 has NO version
# suffix (the file IS the SONAME), so unlike libstdc++.so.6.0.X we must NOT add
# a libm.so.6 symlink -- ln -sf libm.so.6 -> libm.so.6 is a self-loop that
# overwrites the real file and breaks ldso ("cc1: cannot open libm.so.6").
# cp_lib already lays down the SONAME link if the real name differs from it.
LIBM_REAL="$(readlink -f "$("$GXX_BIN" -print-file-name=libm.so.6)")"
if [ -n "$LIBM_REAL" ] && [ -f "$LIBM_REAL" ]; then
    cp_lib "$LIBM_REAL"
    ln -sf "$(basename "$LIBM_REAL")" "$ROOT/usr/lib/libm.so"
fi

# hello.cpp source: STL (vector/string) + exception (throw/catch) smoke.  No
# <thread>/pthread (separate batch).  No -fno-pie: like hello.c, the gcc/g++
# default (PIE) is what we want -- kernel PIE batch 1 loads ET_DYN main.
cat > "$ROOT/hello.cpp" << 'EOF'
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

static void throw_test() { throw std::runtime_error("caught"); }

int main() {
    std::vector<std::string> words{"Hello", "from", "G++!"};
    for (const auto& w : words) std::cout << w << ' ';
    std::cout << '\n';
    try {
        throw_test();
    } catch (const std::exception& e) {
        std::cout << "exception: " << e.what() << '\n';
    }
    return 0;
}
EOF

# hello.cpp's #include closure via g++ -H.  The host g++ resolves <iostream>/
# <vector>/<string> + libstdc++'s <bits/*> + the C headers already staged; we
# install exactly those files at their absolute paths (iostream pulls a few
# hundred headers but still a small slice of the full C++ include tree).
"$GXX_BIN" -H -fsyntax-only "$ROOT/hello.cpp" 2>&1 >/dev/null \
    | sed -E 's/^[. ]+//' | grep -v '^$' | sort -u | while read -r h; do
    [ -f "$h" ] || continue
    install -Dm0644 "$h" "$ROOT$h"
done

echo "[extract] GCC toolchain subset staged at $ROOT (GCC_BIN=$GCC_BIN, GXX_BIN=$GXX_BIN)"
du -sh "$ROOT"
echo "[extract] binaries:"; ls "$ROOT/usr/bin"
echo "[extract] /usr/lib (.so + crt):"; ls "$ROOT/usr/lib"
echo "[extract] $GCC_PROG_DIR:"; ls "$ROOT$GCC_PROG_DIR"
echo "[extract] $GCC_LIB_DIR:"; ls "$ROOT$GCC_LIB_DIR"
