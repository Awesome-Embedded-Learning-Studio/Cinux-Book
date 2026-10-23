#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/../.."
status=0

while IFS= read -r offender; do
    echo "net decoupling: protocol stack reaches into hardware drivers: $offender"
    status=1
done < <(grep -rn -E '#include ["<](kernel/(driver/(pci|usb|ahci|msi)|interrupt)/|.*e1000)' kernel/net/ || true)

while IFS= read -r offender; do
    echo "net decoupling: device family climbs into the protocol stack: $offender"
    status=1
done < <(grep -rn '#include "kernel/net/' kernel/driver/net/ || true)

exit "$status"
