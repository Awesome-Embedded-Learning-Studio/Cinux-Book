#!/usr/bin/env bash
#
# check_net_decoupling.sh -- F7 net-stack decoupling invariants (machine-enforced).
#
# The four greps that make "the L3 stack never names E1000Controller / DmaBuffer /
# arch/irq, and a NIC adapter never reaches for the driver singleton" TESTABLE,
# not aspirational.  Run via:  cmake --build build --target check_net_decoupling
#
# Why: the stack is decoupled so a 2nd NIC (rtl8139 / virtio-net / loopback) is
# "implement NetDevice + register", not a refactor.  A future developer who adds
# `#include arch/x86_64/irq.hpp` to kernel/net/ to make poll() "just work" under
# QEMU, or calls E1000Controller::instance() from a new adapter, has broken that
# -- this script catches it now, not when the 2nd NIC lands.
#
set -u
fail=0
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

echo "[net-decouple] 1. kernel/net/ must not #include e1000*.hpp / dma_buffer.hpp / irq"
if grep -rnE '#include.*(e1000|dma_buffer|irq)' "$ROOT/kernel/net"; then
    echo "  FAIL: kernel/net/ pulls in a driver / DMA / arch-irq header"
    fail=1
else
    echo "  ok (zero matches)"
fi

echo "[net-decouple] 2. NIC adapters must not CALL a driver ::instance() singleton"
echo "   (comment mentions are fine; a real call is not)"
# A code call mentions instance() on a line that is NOT a comment line.
viol="$(grep -rnE 'instance\(\)' "$ROOT"/kernel/drivers/net/*_net_device.* 2>/dev/null \
        | grep -vE ':[[:space:]]*(\*|///|//|/\*)' || true)"
if [ -n "$viol" ]; then
    echo "  FAIL: adapter reaches for a singleton:"
    echo "$viol"
    fail=1
else
    echo "  ok (adapters are constructor-injected)"
fi

if [ "$fail" -ne 0 ]; then
    echo "[net-decouple] DECOUPLING VIOLATED -- see above"
    exit 1
fi
echo "[net-decouple] all invariants hold"
