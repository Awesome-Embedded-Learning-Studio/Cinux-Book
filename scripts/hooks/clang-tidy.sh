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

# Diagnostics from system headers are tool noise: the toolchain cannot
# always parse host C++ headers under every project compile mode (the
# atomic<long double> error under the no-SSE mode is the standing case).
# Judge only diagnostics naming project files.
if clang-tidy --quiet -p build "$@" 2>&1 \
        | grep -E "^/home/charliechen/Cinux/[^ ]*: (error|warning):" ; then
    exit 1
fi
exit 0
