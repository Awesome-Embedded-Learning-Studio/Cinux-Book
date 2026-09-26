#!/usr/bin/env bash
set -u

if [ ! -f build/compile_commands.json ]; then
    echo "[clang-tidy] build/compile_commands.json not found, run cmake first (skipping)"
    exit 0
fi

major=$(clang-tidy --version | sed -n 's/^ *LLVM version \([0-9][0-9]*\).*/\1/p')
if [ "$major" != "22" ]; then
    echo "[clang-tidy] expected LLVM major 22, got ${major:-unknown} (refusing)"
    exit 1
fi

exec clang-tidy --quiet -p build "$@"
