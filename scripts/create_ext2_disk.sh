#!/usr/bin/env bash
# Bakes the teaching ext2 volume: 16 MiB, 1 KiB blocks, no features,
# 512 inodes, seed files from scripts/ext2_seed/. The dynamic pair
# (interpreter + dynamic hello) installs by default when a musl toolchain
# is reachable, so a plain rebuild yields a volume the interpreter station
# can fire without extra setup; CINUX_LDSO/CINUX_HELLO_DYN override, and a
# machine without musl-gcc just skips the pair with one notice line.
set -euo pipefail

image=$1
script_dir="$(cd "$(dirname "$0")" && pwd)"
seed_dir="${script_dir}/ext2_seed"
build_dir="${CINUX_BUILD_DIR:-$(cd "${script_dir}/.." && pwd)/build}"

is_elf() {
    [ -f "$1" ] && [ "$(head -c 4 "$1" | od -An -tx1 | tr -d ' \n')" = "7f454c46" ]
}

# The glibc ecosystem volume bakes from one assembled directory tree:
# seed files, the glibc BusyBox with its hardlinked applets, the teaching
# ELF set, the glibc runtime pair BusyBox needs beyond the toolchain
# closure, and the as/ld toolchain staging tree merged on top. mkfs.ext2 -d
# preserves hardlinks (one busybox inode under every applet name) and
# SONAME symlinks, which the per-file debugfs road below cannot express at
# this scale. 192 MiB / 16384 inodes follows the self-host station's volume
# calibre.
bake_glibc_volume() {
    local staging="${CINUX_TOOLCHAIN_STAGING:-${build_dir}/toolchain-staging}"
    if [ ! -x "${staging}/usr/bin/as" ] || [ ! -x "${staging}/usr/bin/ld" ]; then
        echo "[ext2-disk] toolchain staging missing at ${staging}; build target toolchain_staging first" >&2
        exit 1
    fi
    local assemble="${build_dir}/rootfs-assemble"
    rm -rf "$assemble"
    mkdir -p "$assemble"/bin "$assemble"/sbin "$assemble"/etc "$assemble"/dev \
             "$assemble"/lib "$assemble"/lib64 "$assemble"/proc "$assemble"/root \
             "$assemble"/tmp "$assemble"/usr "$assemble"/var
    install -m 0644 "${seed_dir}/motd.txt"     "$assemble/etc/motd"
    install -m 0644 "${seed_dir}/passwd"       "$assemble/etc/passwd"
    install -m 0644 "${seed_dir}/group"        "$assemble/etc/group"
    install -m 0644 "${seed_dir}/inittab"      "$assemble/etc/inittab"
    install -m 0644 "${seed_dir}/resolv.conf"  "$assemble/etc/resolv.conf"
    install -m 0644 "${seed_dir}/hosts"        "$assemble/etc/hosts"
    install -m 0644 "${seed_dir}/fstab"        "$assemble/etc/fstab"
    install -m 0755 "${seed_dir}/smp-stress.sh" "$assemble/etc/smp-stress.sh"
    install -m 0644 "${seed_dir}/hello.txt"    "$assemble/hello.txt"
    cp "${busybox}" "$assemble/bin/busybox"
    local applet
    for applet in cat ls stty script id uname ping ifconfig nslookup netstat \
                  echo whoami pwd true false sleep env hostname wc ps free \
                  mount umount mkdir sh; do
        ln "$assemble/bin/busybox" "$assemble/bin/${applet}"
    done
    ln "$assemble/bin/busybox" "$assemble/sbin/init"
    local seat_pair
    for seat_pair in "hello:${CINUX_HELLO_ELF:-}" "mapper:${CINUX_MAPPER_ELF:-}" \
                     "sigdemo:${CINUX_SIGDEMO_ELF:-}" "sigcatch:${CINUX_SIGCATCH_ELF:-}" \
                     "nxdemo:${CINUX_NXDEMO_ELF:-}" "efaulter:${CINUX_EFAULTER_ELF:-}" \
                     "smepdemo:${CINUX_SMEPDEMO_ELF:-}" "aslrdemo:${CINUX_ASLRDEMO_ELF:-}" \
                     "tlsdemo:${CINUX_TLSDEMO_ELF:-}" "threaddemo:${CINUX_THREADDEMO_ELF:-}" \
                     "ttydemo:${CINUX_TTYDEMO_ELF:-}" "smpstress:${CINUX_SMPSTRESS_ELF:-}"; do
        local seat_name="${seat_pair%%:*}" seat_src="${seat_pair#*:}"
        [ -n "$seat_src" ] && [ -f "$seat_src" ] && install -m 0755 "$seat_src" "$assemble/bin/${seat_name}"
    done
    local library library_path
    for library in libm.so.6 libresolv.so.2; do
        library_path="$(gcc -print-file-name="$library")"
        if ! is_elf "$library_path"; then
            echo "[ext2-disk] missing runtime library: $library" >&2
            exit 1
        fi
        cp "$library_path" "$assemble/lib/${library}"
    done
    local hello_glibc="${CINUX_HELLO_GLIBC:-}"
    if [ -z "$hello_glibc" ]; then
        mkdir -p "${build_dir}/glibc"
        hello_glibc="${build_dir}/glibc/hello-glibc"
        if [ ! -f "$hello_glibc" ]; then
            bash "${script_dir}/glibc/build-hello-glibc.sh" "$hello_glibc" >/dev/null
        fi
    fi
    [ -f "$hello_glibc" ] && install -m 0755 "$hello_glibc" "$assemble/bin/hello-glibc"
    local vesper_desk="${CINUX_VESPER_DESK:-}"
    if [ -n "$vesper_desk" ] && [ -f "$vesper_desk" ]; then
        install -m 0755 "$vesper_desk" "$assemble/bin/vesper"
        printf '%s\n' '::respawn:/bin/vesper /dev/fb0 /dev/event0 /dev/event1' \
            >> "$assemble/etc/inittab"
        echo "[ext2-disk] desk seated at /bin/vesper (inittab respawn)"
    else
        echo "[ext2-disk] no desk binary at CINUX_VESPER_DESK; volume boots headless" >&2
    fi
    local ecotest="${CINUX_ECOTEST:-${build_dir}/glibc/ecotest}"
    [ -f "$ecotest" ] && install -m 0755 "$ecotest" "$assemble/bin/ecotest"
    cp -a "${staging}/." "$assemble/"
    truncate -s 192M "$image"
    mkfs.ext2 -q -F -b 1024 -O none -N 16384 -d "$assemble" "$image"
    echo "[ext2-disk] glibc volume baked from ${assemble} (busybox + as/ld toolchain, 192M/16384 inodes)"
}

ldso="${CINUX_LDSO:-}"
if [ -z "$ldso" ]; then
    for candidate in "${HOME}/.local/musl/lib/libc.so" "/usr/lib/x86_64-linux-musl/libc.so"; do
        if is_elf "$candidate"; then
            ldso="$candidate"
            break
        fi
    done
fi

hello_dyn="${CINUX_HELLO_DYN:-}"
if [ -z "$hello_dyn" ] && [ -n "$ldso" ]; then
    mkdir -p "${build_dir}/musl"
    hello_dyn="${build_dir}/musl/hello-dyn"
    if [ ! -f "$hello_dyn" ]; then
        bash "${script_dir}/musl/build-hello-dyn.sh" "$hello_dyn" >/dev/null
    fi
fi

busybox="${CINUX_BUSYBOX:-}"
libc_flavor="${CINUX_LIBC:-glibc}"
if [ "$libc_flavor" != glibc ] && [ "$libc_flavor" != musl ]; then
    echo "[ext2-disk] unknown CINUX_LIBC: $libc_flavor" >&2
    exit 1
fi
if [ -z "$busybox" ] && [ "$libc_flavor" = "glibc" ]; then
    busybox="${build_dir}/busybox-glibc/busybox"
    if [ ! -f "$busybox" ] && [ -d "${HOME}/.local/src/busybox-1.38.0" ]; then
        bash "${script_dir}/build-busybox-glibc.sh" "$busybox" >/dev/null || busybox=""
    fi
fi
if [ -z "$busybox" ] && [ "$libc_flavor" != "glibc" ]; then
    busybox="${build_dir}/busybox/busybox"
    if [ ! -f "$busybox" ] && [ -d "${HOME}/.local/src/busybox-1.38.0" ]; then
        bash "${script_dir}/build-busybox.sh" "$busybox" >/dev/null || busybox=""
    fi
fi
is_elf "$busybox" || busybox=""
if [ "$libc_flavor" = glibc ] && [ -z "$busybox" ]; then
    echo '[ext2-disk] glibc BusyBox missing; run bash user/rootfs/busybox/build.sh build' >&2
    exit 1
fi

if [ "$libc_flavor" = glibc ]; then
    bake_glibc_volume
    exit 0
fi

truncate -s 16M "$image"
mkfs.ext2 -q -F -b 1024 -O none -N 512 "$image"
debugfs -w -R 'mkdir /etc' "$image" >/dev/null 2>&1
debugfs -w -R 'mkdir /bin' "$image" >/dev/null 2>&1
debugfs -w -R 'mkdir /dev' "$image" >/dev/null 2>&1
debugfs -w -R 'mkdir /lib' "$image" >/dev/null 2>&1
debugfs -w -R 'mkdir /proc' "$image" >/dev/null 2>&1
debugfs -w -R 'mkdir /root' "$image" >/dev/null 2>&1
debugfs -w -R 'mkdir /sbin' "$image" >/dev/null 2>&1
debugfs -w -R 'mkdir /tmp' "$image" >/dev/null 2>&1
debugfs -w -R 'mkdir /var' "$image" >/dev/null 2>&1
debugfs -w -R "write ${seed_dir}/motd.txt /etc/motd" "$image" >/dev/null 2>&1
debugfs -w -R "write ${seed_dir}/passwd /etc/passwd" "$image" >/dev/null 2>&1
debugfs -w -R "write ${seed_dir}/group /etc/group" "$image" >/dev/null 2>&1
debugfs -w -R "write ${seed_dir}/inittab /etc/inittab" "$image" >/dev/null 2>&1
debugfs -w -R "write ${seed_dir}/hello.txt /hello.txt" "$image" >/dev/null 2>&1
debugfs -w -R "write ${seed_dir}/smp-stress.sh /etc/smp-stress.sh" "$image" >/dev/null 2>&1
debugfs -w -R "write ${seed_dir}/resolv.conf /etc/resolv.conf" "$image" >/dev/null 2>&1
debugfs -w -R "write ${seed_dir}/hosts /etc/hosts" "$image" >/dev/null 2>&1
debugfs -w -R "write ${seed_dir}/fstab /etc/fstab" "$image" >/dev/null 2>&1
if [ -n "$busybox" ]; then
    debugfs -w -R "write ${busybox} /bin/busybox" "$image" >/dev/null 2>&1
    for applet in cat ls stty script id uname ping ifconfig nslookup netstat echo whoami pwd true false sleep env hostname wc ps free mount umount mkdir sh; do
        debugfs -w -R "ln /bin/busybox /bin/${applet}" "$image" >/dev/null 2>&1
    done
    debugfs -w -R 'ln /bin/busybox /sbin/init' "$image" >/dev/null 2>&1
    debugfs -w -R 'set_inode_field /bin/busybox links_count 27' "$image" >/dev/null 2>&1
    echo "[ext2-disk] busybox seated as /bin/sh from ${busybox}"
elif [ -f "${CINUX_SHELL_ELF:-}" ]; then
    debugfs -w -R "write ${CINUX_SHELL_ELF} /bin/sh" "$image" >/dev/null 2>&1
    echo "[ext2-disk] teaching shell seated (no busybox source tree)"
fi
if [ -f "${CINUX_HELLO_ELF:-}" ]; then
    debugfs -w -R "write ${CINUX_HELLO_ELF} /bin/hello" "$image" >/dev/null 2>&1
fi
if [ -f "${CINUX_MAPPER_ELF:-}" ]; then
    debugfs -w -R "write ${CINUX_MAPPER_ELF} /bin/mapper" "$image" >/dev/null 2>&1
fi
if [ -f "${CINUX_SIGDEMO_ELF:-}" ]; then
    debugfs -w -R "write ${CINUX_SIGDEMO_ELF} /bin/sigdemo" "$image" >/dev/null 2>&1
fi
if [ -f "${CINUX_SIGCATCH_ELF:-}" ]; then
    debugfs -w -R "write ${CINUX_SIGCATCH_ELF} /bin/sigcatch" "$image" >/dev/null 2>&1
fi
if [ -f "${CINUX_NXDEMO_ELF:-}" ]; then
    debugfs -w -R "write ${CINUX_NXDEMO_ELF} /bin/nxdemo" "$image" >/dev/null 2>&1
fi
if [ -f "${CINUX_EFAULTER_ELF:-}" ]; then
    debugfs -w -R "write ${CINUX_EFAULTER_ELF} /bin/efaulter" "$image" >/dev/null 2>&1
fi
if [ -f "${CINUX_SMEPDEMO_ELF:-}" ]; then
    debugfs -w -R "write ${CINUX_SMEPDEMO_ELF} /bin/smepdemo" "$image" >/dev/null 2>&1
fi
if [ -f "${CINUX_ASLRDEMO_ELF:-}" ]; then
    debugfs -w -R "write ${CINUX_ASLRDEMO_ELF} /bin/aslrdemo" "$image" >/dev/null 2>&1
fi
if [ -f "${CINUX_TLSDEMO_ELF:-}" ]; then
    debugfs -w -R "write ${CINUX_TLSDEMO_ELF} /bin/tlsdemo" "$image" >/dev/null 2>&1
fi
if [ -f "${CINUX_THREADDEMO_ELF:-}" ]; then
    debugfs -w -R "write ${CINUX_THREADDEMO_ELF} /bin/threaddemo" "$image" >/dev/null 2>&1
fi
if [ -f "${CINUX_TTYDEMO_ELF:-}" ]; then
    debugfs -w -R "write ${CINUX_TTYDEMO_ELF} /bin/ttydemo" "$image" >/dev/null 2>&1
fi
if [ -f "${CINUX_SMPSTRESS_ELF:-}" ]; then
    debugfs -w -R "write ${CINUX_SMPSTRESS_ELF} /bin/smpstress" "$image" >/dev/null 2>&1
fi
if [ -f "$ldso" ] && [ "$libc_flavor" != "glibc" ]; then
    debugfs -w -R "write ${ldso} /lib/ld-musl-x86_64.so.1" "$image" >/dev/null 2>&1
    echo "[ext2-disk] interpreter installed from ${ldso}"
elif [ "$libc_flavor" != "glibc" ]; then
    echo "[ext2-disk] no musl toolchain found; /lib stays empty (dynamic station needs it)"
fi
if [ -f "$hello_dyn" ] && [ "$libc_flavor" != "glibc" ]; then
    debugfs -w -R "write ${hello_dyn} /bin/hello-dyn" "$image" >/dev/null 2>&1
fi

glibc_dir="/usr/lib"
ldso_glibc="${CINUX_GLIBC_LDSO:-/lib64/ld-linux-x86-64.so.2}"
libc_glibc="${CINUX_GLIBC_LIBC:-$(gcc -print-file-name=libc.so.6)}"
hello_glibc="${CINUX_HELLO_GLIBC:-}"
if [ -z "$hello_glibc" ] && [ -f "$ldso_glibc" ]; then
    mkdir -p "${build_dir}/glibc"
    hello_glibc="${build_dir}/glibc/hello-glibc"
    if [ ! -f "$hello_glibc" ]; then
        bash "${script_dir}/glibc/build-hello-glibc.sh" "$hello_glibc" >/dev/null
    fi
fi
if [ -f "$ldso_glibc" ]; then
    ldso_glibc="$(readlink -f "$ldso_glibc")"
    libc_glibc="$(readlink -f "$libc_glibc")"
    debugfs -w -R 'mkdir /lib64' "$image" >/dev/null 2>&1
    debugfs -w -R "write ${ldso_glibc} /lib64/ld-linux-x86-64.so.2" "$image" >/dev/null 2>&1
    debugfs -w -R "write ${libc_glibc} /lib/libc.so.6" "$image" >/dev/null 2>&1
    for library in libm.so.6 libresolv.so.2; do
        library_path="$(gcc -print-file-name="$library")"
        if ! is_elf "$library_path"; then
            echo "[ext2-disk] missing runtime library: $library" >&2
            exit 1
        fi
        debugfs -w -R "write ${library_path} /lib/${library}" "$image" >/dev/null 2>&1
    done
    echo "[ext2-disk] glibc runtime seated from ${ldso_glibc}"
fi
if [ -f "$hello_glibc" ]; then
    debugfs -w -R "write ${hello_glibc} /bin/hello-glibc" "$image" >/dev/null 2>&1
fi
if [ -f "${CINUX_ECOTEST:-${build_dir}/glibc/ecotest}" ]; then
    debugfs -w -R "write ${CINUX_ECOTEST:-${build_dir}/glibc/ecotest} /bin/ecotest" "$image" >/dev/null 2>&1
fi
