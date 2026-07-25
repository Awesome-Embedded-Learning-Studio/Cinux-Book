#!/bin/bash
# qemu_test_wrapper.sh — Run QEMU and map isa-debug-exit codes to pass/fail
#
# QEMU's isa-debug-exit device encodes: exit_code = (value << 1) | 1
#   Kernel writes 0 → QEMU exits 1 → test SUCCESS
#   Kernel writes 1 → QEMU exits 3 → test FAILURE (a unit test failed)
#   Kernel panic (exception_handlers.cpp::panic / kprintf.cpp::kpanic) writes a
#   distinct value so CI fails FAST (no cli;hlt → timeout stall) and the exit
#   code names the cause:
#     value = vector + 2  (exception fault; 0/1 reserved) → exit (value<<1)|1
#       #DE(0)→5  #DF(8)→21  #GP(13)→31  #PF(14)→33 ...
#     value = 64           (generic kpanic / assertion)  → exit 129
#   Decode a panic exit: vector = (rc-1)/2 - 2  (rc 129 = generic kpanic).
#
# Usage: qemu_test_wrapper.sh <qemu> <args...>

"$@"
rc=$?

if [ "$rc" -eq 1 ]; then
    # Kernel wrote 0 (success) → QEMU exit 1
    exit 0
elif [ "$rc" -eq 3 ]; then
    # Kernel wrote 1 (failure) → QEMU exit 3
    exit 1
elif [ "$rc" -eq 129 ]; then
    echo "KERNEL PANIC: kpanic/assertion (QEMU exit $rc)"
    exit "$rc"
else
    # Exception-fault panic (isa-debug-exit) or a raw QEMU crash. Decode the
    # vector when it fits the value=vector+2 scheme; otherwise report raw.
    vec=$(( (rc - 1) / 2 - 2 ))
    if [ "$rc" -ge 5 ] && [ "$rc" -le 67 ] && [ "$vec" -ge 0 ] && [ "$vec" -le 31 ]; then
        echo "KERNEL PANIC: exception vector $vec (QEMU exit $rc)"
    else
        echo "QEMU unexpected exit code: $rc"
    fi
    exit "$rc"
fi
