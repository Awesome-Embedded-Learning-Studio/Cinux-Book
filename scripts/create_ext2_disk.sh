#!/usr/bin/env bash
# Bakes the teaching ext2 volume: 16 MiB, 1 KiB blocks, no features,
# 512 inodes, seed files from scripts/ext2_seed/.
set -euo pipefail

image=$1
seed_dir="$(cd "$(dirname "$0")" && pwd)/ext2_seed"

truncate -s 16M "$image"
mkfs.ext2 -q -F -b 1024 -O none -N 512 "$image"
debugfs -w -R 'mkdir /etc' "$image" >/dev/null 2>&1
debugfs -w -R 'mkdir /bin' "$image" >/dev/null 2>&1
debugfs -w -R "write ${seed_dir}/motd.txt /etc/motd" "$image" >/dev/null 2>&1
debugfs -w -R "write ${seed_dir}/hello.txt /hello.txt" "$image" >/dev/null 2>&1
if [ -f "${CINUX_SHELL_ELF:-}" ]; then
    debugfs -w -R "write ${CINUX_SHELL_ELF} /bin/sh" "$image" >/dev/null 2>&1
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
