#!/usr/bin/env bash
# F-QA Q1-4: fail if the QEMU kernel test count dropped below the baseline.
# Prevents silent test loss — a RUN_TEST removed/disabled by accident, or a
# whole module's tests silently skipped reads as "green" without this gate.
# The kernel-tests matrix (Q1-3) runs this per config; every config must hold
# the floor.
#
# Usage: check_test_count.sh <serial.log> [baseline]
# Raise the baseline (here or via CINUX_TEST_BASELINE) ONLY when a drop is
# intentional; an accidental drop fails CI.
set -euo pipefail

LOG="${1:-build/serial.log}"
BASELINE="${2:-${CINUX_TEST_BASELINE:-875}}"

if [ ! -f "$LOG" ]; then
    echo "[check_test_count] no serial log at '$LOG' — nothing to check" >&2
    exit 0
fi

# run-kernel-test prints: "=== Tests: <n> passed, <m> failed ==="
# serial.log carries QEMU ANSI escapes, so grep would otherwise treat it as a
# binary file and only emit "binary file matches" (GOTCHA#2). -a forces text.
LINE=$(grep -aE 'Tests: [0-9]+ passed, [0-9]+ failed' "$LOG" | tail -n 1 || true)
if [ -z "$LINE" ]; then
    echo "[check_test_count] FAIL: could not find 'Tests: N passed, M failed' in $LOG" >&2
    exit 1
fi

PASSED=$(printf '%s' "$LINE" | grep -aoE '[0-9]+ passed' | grep -aoE '[0-9]+' | head -n 1)
FAILED=$(printf '%s' "$LINE" | grep -aoE '[0-9]+ failed' | grep -aoE '[0-9]+' | head -n 1)

echo "[check_test_count] passed=$PASSED failed=$FAILED baseline=$BASELINE"

if [ "$FAILED" -ne 0 ]; then
    echo "[check_test_count] FAIL: $FAILED test(s) failed (already reported above)" >&2
    exit 1
fi
if [ "$PASSED" -lt "$BASELINE" ]; then
    echo "[check_test_count] FAIL: passed ($PASSED) < baseline ($BASELINE) — tests were silently removed/disabled. Raise CINUX_TEST_BASELINE only if the drop is intentional." >&2
    exit 1
fi

echo "[check_test_count] OK: $PASSED >= $BASELINE"
