#!/usr/bin/env bash
set -uo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/../.."
check_jobs="${CINUX_CHECK_JOBS:-4}"
logs="${CINUX_CHECK_LOG_DIR:-$(mktemp -d "${TMPDIR:-/tmp}/cinux-acceptance-XXXXXX")}"
mkdir -p "$logs"

run_segment() {
    local name="$1"
    shift
    echo "[acceptance] RUN ${name}"
    echo "RUNNING" >"${logs}/${name}.status"
    if ! "$@" >"${logs}/${name}.log" 2>&1; then
        echo "FAILED" >"${logs}/${name}.status"
        return 1
    fi
    echo "OK" >"${logs}/${name}.status"
}

failed=0

export CINUX_CHECK_JOBS="$check_jobs"
run_segment default bash scripts/checks/run-cell.sh kernel Release none build || failed=1
run_segment asan bash scripts/checks/run-cell.sh host Release asan build-asan || failed=1
run_segment tsan bash scripts/checks/run-cell.sh host Release tsan build-tsan || failed=1
run_segment checks bash scripts/checks/run-cell.sh kernel Release checks build-checks || failed=1
run_segment ecosystem bash scripts/checks/run-cell.sh ecosystem Release none build || failed=1
run_segment ecosystem-checks bash scripts/checks/run-cell.sh ecosystem Release checks build-checks || failed=1

for name in default asan tsan checks ecosystem ecosystem-checks; do
    status="$(cat "${logs}/${name}.status" 2>/dev/null || echo FAILED)"
    if [[ "${status}" != "OK" ]]; then
        echo "[acceptance] ${name}: FAILED — full log follows"
        cat "${logs}/${name}.log"
    else
        echo "[acceptance] ${name}: OK"
    fi
done

if [[ "${failed}" -ne 0 ]]; then
    echo "[acceptance] verdict: FAILED (logs kept in ${logs})"
    exit 1
fi
if [[ -z "${CINUX_CHECK_LOG_DIR:-}" ]]; then
    rm -rf "${logs}"
else
    echo "[acceptance] logs kept in ${logs}"
fi
echo "[acceptance] verdict: four trees and both ecosystem cells green"
