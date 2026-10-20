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
build_dir="$(cd "${script_dir}/.." && pwd)/build"

is_elf() {
    [ -f "$1" ] && [ "$(head -c 4 "$1" | od -An -tx1 | tr -d ' \n')" = "7f454c46" ]
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
if [ -z "$busybox" ]; then
    busybox="${build_dir}/busybox/busybox"
    if [ ! -f "$busybox" ] && [ -d "${HOME}/.local/src/busybox-1.38.0" ]; then
        bash "${script_dir}/build-busybox.sh" "$busybox" >/dev/null || busybox=""
    fi
fi
is_elf "$busybox" || busybox=""

truncate -s 16M "$image"
mkfs.ext2 -q -F -b 1024 -O none -N 512 "$image"
debugfs -w -R 'mkdir /etc' "$image" >/dev/null 2>&1
debugfs -w -R 'mkdir /bin' "$image" >/dev/null 2>&1
debugfs -w -R 'mkdir /dev' "$image" >/dev/null 2>&1
debugfs -w -R 'mkdir /lib' "$image" >/dev/null 2>&1
debugfs -w -R 'mkdir /proc' "$image" >/dev/null 2>&1
debugfs -w -R "write ${seed_dir}/motd.txt /etc/motd" "$image" >/dev/null 2>&1
debugfs -w -R "write ${seed_dir}/hello.txt /hello.txt" "$image" >/dev/null 2>&1
if [ -n "$busybox" ]; then
    debugfs -w -R "write ${busybox} /bin/busybox" "$image" >/dev/null 2>&1
    debugfs -w -R "write ${busybox} /bin/sh" "$image" >/dev/null 2>&1
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
if [ -f "${CINUX_TLSDEMO_ELF:-}" ]; then
    debugfs -w -R "write ${CINUX_TLSDEMO_ELF} /bin/tlsdemo" "$image" >/dev/null 2>&1
fi
if [ -f "${CINUX_THREADDEMO_ELF:-}" ]; then
    debugfs -w -R "write ${CINUX_THREADDEMO_ELF} /bin/threaddemo" "$image" >/dev/null 2>&1
fi
if [ -f "${CINUX_TTYDEMO_ELF:-}" ]; then
    debugfs -w -R "write ${CINUX_TTYDEMO_ELF} /bin/ttydemo" "$image" >/dev/null 2>&1
fi
if [ -f "$ldso" ]; then
    debugfs -w -R "write ${ldso} /lib/ld-musl-x86_64.so.1" "$image" >/dev/null 2>&1
    echo "[ext2-disk] interpreter installed from ${ldso}"
else
    echo "[ext2-disk] no musl toolchain found; /lib stays empty (dynamic station needs it)"
fi
if [ -f "$hello_dyn" ]; then
    debugfs -w -R "write ${hello_dyn} /bin/hello-dyn" "$image" >/dev/null 2>&1
fi
