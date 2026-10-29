#!/usr/bin/env bash
# Stages one host GCC/G++ toolchain, binutils, runtime libraries, crt
# artifacts and the C/C++ smoke header closures at their absolute paths.
# The guest builds and runs default-PIE C and C++ programs from this tree.
# Inputs and script contents fingerprint the cached staging directory.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/.." && pwd)"
out="${1:-${repo}/build/toolchain-staging}"
meta_dir="${out}.meta"
mkdir -p "$meta_dir"

gcc_bin="${CINUX_GCC_BIN:-gcc}"
command -v "$gcc_bin" >/dev/null 2>&1 || {
    echo "[toolchain-staging] gcc not found on PATH: $gcc_bin" >&2
    exit 1
}
as_bin="$(command -v as)"
ld_bin="$(command -v ld)"
ldso_bin="$(readlink -f /lib64/ld-linux-x86-64.so.2)"

gxx_bin="${CINUX_GXX_BIN:-${gcc_bin/gcc/g++}}"
command -v "$gxx_bin" >/dev/null 2>&1 || {
    echo "[toolchain-staging] matching g++ not found: $gxx_bin" >&2
    exit 1
}
if [ "$("$gcc_bin" -dumpfullversion)" != "$("$gxx_bin" -dumpfullversion)" ] ||
   [ "$("$gcc_bin" -dumpmachine)" != "$("$gxx_bin" -dumpmachine)" ]; then
    echo "[toolchain-staging] gcc/g++ version or target mismatch" >&2
    exit 1
fi
gcc_lib_dir="$(dirname "$("$gcc_bin" -print-file-name=libgcc.a)")"
cc1="$("$gcc_bin" -print-prog-name=cc1)"
cc1plus="$("$gxx_bin" -print-prog-name=cc1plus)"
collect2="$("$gcc_bin" -print-prog-name=collect2)"
for program in "$cc1" "$cc1plus" "$collect2"; do
    if [ ! -x "$program" ]; then
        echo "[toolchain-staging] required compiler program missing: $program" >&2
        exit 1
    fi
done
libstdcxx="$(readlink -f "$("$gxx_bin" -print-file-name=libstdc++.so.6)")"
libatomic="$(readlink -f "$("$gcc_bin" -print-file-name=libatomic.so)")"
libm="$(readlink -f "$("$gxx_bin" -print-file-name=libm.so.6)")"
for library in "$libstdcxx" "$libm" "$libatomic"; do
    if [ ! -f "$library" ]; then
        echo "[toolchain-staging] required library missing: $library" >&2
        exit 1
    fi
done

header_inputs() {
    "$gcc_bin" -H -fsyntax-only "${here}/ext2_seed/hello.c" 2>&1 >/dev/null
    "$gxx_bin" -H -fsyntax-only "${here}/ext2_seed/hello.cpp" 2>&1 >/dev/null
}

collect_inputs() {
    printf '%s\n' "$as_bin" "$ld_bin" "$ldso_bin" "$(command -v "$gcc_bin")"
    printf '%s\n' "$cc1" "$cc1plus" "$collect2" "$(command -v "$gxx_bin")" "$libstdcxx" "$libm" "$libatomic"
    ldd "$as_bin" "$ld_bin" "$cc1" "$cc1plus" "$collect2" \
        "$(command -v "$gcc_bin")" "$(command -v "$gxx_bin")" "$libstdcxx" 2>/dev/null \
        | grep '=> /' | awk '{print $3}' | sort -u
    local name
    for name in crt1.o Scrt1.o crti.o crtn.o crtbegin.o crtbeginS.o crtbeginT.o \
                crtend.o crtendS.o crtfastmath.o libgcc.a libgcc_eh.a \
                libgcc_s.so libgcc_s.so.1 libc.so libc_nonshared.a libc.a \
                libgcc_s_asneeded.so libatomic_asneeded.so libatomic_asneeded.a \
                liblto_plugin.so; do
        local path
        path="$("$gcc_bin" -print-file-name="$name")"
        if [ "$path" != "$name" ] && [ -e "$path" ]; then
            printf '%s\n' "$(readlink -f "$path")"
        fi
    done
    printf '%s\n' "${here}/ext2_seed/hello.c" "${here}/ext2_seed/hello.cpp" "${here}/ext2_seed/execprobe.c" "${here}/ext2_seed/sendfileprobe.c" /usr/include/stdc-predef.h
    header_inputs | sed -E 's/^[. ]+//' | while read -r header; do
        [ ! -f "$header" ] || printf '%s\n' "$header"
    done
}

script_abs="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")"
fingerprint="$({ collect_inputs | sort -u | xargs sha256sum; sha256sum "$script_abs"; } | sha256sum | cut -d ' ' -f 1)"
if [ -d "$out" ] && [ -f "${meta_dir}/.inputs" ] &&
   [ "$(cat "${meta_dir}/.inputs")" = "$fingerprint" ]; then
    echo "[toolchain-staging] cache hit, keeping ${out}"
    exit 0
fi

work="$(mktemp -d /tmp/cinux-toolchain-staging.XXXXXX)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work"/usr/bin "$work"/usr/lib "$work"/lib64 "$work$gcc_lib_dir"

cp -aL "$as_bin" "$work/usr/bin/as"
cp -aL "$ld_bin" "$work/usr/bin/ld"
cp -aL "$ldso_bin" "$work/lib64/ld-linux-x86-64.so.2"

cp_lib() {
    local src="$1"
    [ -f "$src" ] || return 0
    local base soname
    base="$(basename "$src")"
    cp -aL "$src" "$work/usr/lib/"
    # Linker scripts name host multiarch paths such as /lib/x86_64-linux-gnu.
    # Keep those paths as well as the guest's flat runtime search directory.
    if [ "$src" != "/usr/lib/$base" ]; then
        mkdir -p "$work$(dirname "$src")"
        ln -f "$work/usr/lib/$base" "$work$src"
    fi
    soname="$(readelf -d "$src" 2>/dev/null | sed -n 's/.*SONAME.*\[\(.*\)\].*/\1/p' | head -1)"
    if [ -n "$soname" ] && [ "$soname" != "$base" ] && [ ! -e "$work/usr/lib/$soname" ]; then
        ln -sf "$base" "$work/usr/lib/$soname"
    fi
}

ldd "$as_bin" "$ld_bin" "$cc1" "$cc1plus" "$collect2" \
    "$(command -v "$gcc_bin")" "$(command -v "$gxx_bin")" "$libstdcxx" 2>/dev/null | grep '=> /' | awk '{print $3}' | sort -u | while read -r lib; do
    cp_lib "$lib"
done

copy_gcc_file() {
    local name="$1" path
    path="$("$gcc_bin" -print-file-name="$name")"
    [ "$path" != "$name" ] || return 0
    [ -e "$path" ] || return 0
    mkdir -p "$work$(dirname "$path")"
    cp -aL "$path" "$work$path"
}

for name in crt1.o Scrt1.o crti.o crtn.o crtbegin.o crtbeginS.o crtbeginT.o \
            crtend.o crtendS.o crtfastmath.o libgcc.a libgcc_eh.a \
            libgcc_s.so libgcc_s.so.1 libc.so libc_nonshared.a libc.a \
            libgcc_s_asneeded.so libatomic_asneeded.so libatomic_asneeded.a; do
    copy_gcc_file "$name"
done

"$gcc_bin" -S -o "$work/hello.s" "${here}/ext2_seed/hello.c"
cp "${here}/ext2_seed/hello.c" "$work/hello.c"

cp "${here}/ext2_seed/hello.cpp" "$work/hello.cpp"
mkdir -p "$work/bin"
"$gcc_bin" -O2 "${here}/ext2_seed/execprobe.c" -o "$work/bin/execprobe"
"$gcc_bin" -O2 "${here}/ext2_seed/sendfileprobe.c" -o "$work/bin/sendfileprobe"
for program in "$cc1" "$cc1plus" "$collect2"; do
    mkdir -p "$work$(dirname "$program")"
    cp -aL "$program" "$work$program"
done
cp -aL "$(command -v "$gcc_bin")" "$work/usr/bin/gcc"
cp -aL "$(command -v "$gxx_bin")" "$work/usr/bin/g++"
copy_gcc_file liblto_plugin.so
cp_lib "$libstdcxx"
cp_lib "$libm"
cp_lib "$libatomic"
ln -sf "$(basename "$libatomic")" "$work/usr/lib/libatomic.so"
ln -sf "$(basename "$libstdcxx")" "$work/usr/lib/libstdc++.so"
ln -sf "$(basename "$libm")" "$work/usr/lib/libm.so"
header_inputs | sed -E 's/^[. ]+//' | sort -u | while read -r header; do
    [ -f "$header" ] || continue
    install -Dm0644 "$header" "$work$header"
done
install -Dm0644 /usr/include/stdc-predef.h "$work/usr/include/stdc-predef.h"

{
    printf '%s\n' -pie -o /hello
    printf '%s\n' -dynamic-linker /lib64/ld-linux-x86-64.so.2
    printf '%s\n' "$(readlink -f "$("$gcc_bin" -print-file-name=Scrt1.o)")"
    printf '%s\n' "$(readlink -f "$("$gcc_bin" -print-file-name=crti.o)")"
    printf '%s\n' "${gcc_lib_dir}/crtbeginS.o"
    printf '%s\n' /hello.o
    printf '%s\n' -L/usr/lib
    printf '%s\n' "-L$(dirname "$(readlink -f "$("$gcc_bin" -print-file-name=libc.so)")")"
    printf '%s\n' "-L${gcc_lib_dir}"
    printf '%s\n' -lgcc
    printf '%s\n' -lc
    printf '%s\n' "${gcc_lib_dir}/crtendS.o"
    printf '%s\n' "$(readlink -f "$("$gcc_bin" -print-file-name=crtn.o)")"
} > "$work/hello.ldargs"

printf 'GCC_LIB_DIR=%s\n' "$gcc_lib_dir" > "$meta_dir/manifest"
rm -rf "$out"
mv "$work" "$out"
printf '%s\n' "$fingerprint" > "${meta_dir}/.inputs"
echo "[toolchain-staging] GCC/G++ closure staged at $out"
du -sh "$out"
