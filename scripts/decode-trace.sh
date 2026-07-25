#!/bin/bash
# @file scripts/decode-trace.sh
# @brief Resolve raw kernel addresses to demangled function:source:line.
#
# Feeds hex addresses (from argv or extracted from a serial/panic log on stdin)
# to `addr2line -e <elf> -f -C`, which demangles and prints the function and
# source location. Complements the in-kernel KALLSYMS table (F-INFRA I-5): that
# shows (mangled) names live at panic time; this gives exact demangled source
# locations from the host ELF, and works even when the table is empty (first
# build) or an address is outside any named symbol.
#
# Usage:
#   decode-trace.sh 0xffffffff8100004e 0xffffffff810083bc       # default ELF
#   decode-trace.sh build/kernel/big/big_kernel 0xffffffff8100004e
#   decode-trace.sh < build/serial.log                          # extract addrs
#   grep -oiE '0x[0-9a-f]+' panic.log | decode-trace.sh
#
# Default ELF is the test kernel (what run-kernel-test + dev panics run). Pass
# the production kernel or set CINUX_ELF to decode a production backtrace.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
DEFAULT_ELF="${CINUX_ELF:-$PROJECT_ROOT/build/kernel/big/big_kernel_test}"

# Decide ELF vs addresses. First arg is an ELF unless it starts with 0x (an
# address) or there are no args at all (stdin mode).
if [ "$#" -eq 0 ]; then
    ELF="$DEFAULT_ELF"
    addrs=()
    mapfile -t addrs < <(grep -oiE '0x[0-9a-f]+' || true)
elif [[ "$1" == 0x* || "$1" == 0X* ]]; then
    ELF="$DEFAULT_ELF"
    addrs=("$@")
else
    ELF="$1"
    shift
    if [ "$#" -gt 0 ]; then
        addrs=("$@")
    else
        addrs=()
        mapfile -t addrs < <(grep -oiE '0x[0-9a-f]+' || true)
    fi
fi

if [ ! -f "$ELF" ]; then
    echo "decode-trace: ELF not found: $ELF" >&2
    echo "  build first, or pass an explicit path." >&2
    exit 1
fi

if [ "${#addrs[@]}" -eq 0 ]; then
    echo "decode-trace: no addresses found (argv or stdin)." >&2
    exit 0
fi

echo "# decoding ${#addrs[@]} address(es) against $ELF  (-C demangled)"
for a in "${addrs[@]}"; do
    # -f prints the function name, -C demangles; output line 2 is file:line.
    info=$(addr2line -e "$ELF" -f -C "$a" 2>/dev/null || true)
    func=$(printf '%s\n' "$info" | sed -n '1p')
    loc=$(printf '%s\n' "$info" | sed -n '2p')
    printf "%-22s %s  @  %s\n" "$a" "$func" "$loc"
done
