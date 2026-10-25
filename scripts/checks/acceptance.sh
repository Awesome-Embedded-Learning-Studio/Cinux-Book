#!/usr/bin/env bash
set -uo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/../.."
check_jobs="${CINUX_CHECK_JOBS:-4}"
logs="${CINUX_CHECK_LOG_DIR:-$(mktemp -d "${TMPDIR:-/tmp}/cinux-acceptance-XXXXXX")}"
mkdir -p "$logs"

run_segment() {
    local name="$1"
    shift
    echo "RUNNING" >"${logs}/${name}.status"
    if ! "$@" >"${logs}/${name}.log" 2>&1; then
        echo "FAILED" >"${logs}/${name}.status"
        return 1
    fi
    echo "OK" >"${logs}/${name}.status"
}

failed=0

run_segment default bash -e -c '
    cmake -S . -B build -DCINUX_HOST_ASAN=OFF -DCINUX_HOST_TSAN=OFF \
        -DCINUX_LOCKDEP=OFF -DCINUX_UBSAN=OFF
    cmake --build build --parallel '"${check_jobs}"' --target test_host test_kernel boot_image
' || failed=1

run_segment asan bash -e -c '
    cmake -S . -B build-asan -DCINUX_HOST_ASAN=ON -DCINUX_HOST_TSAN=OFF \
        -DCINUX_LOCKDEP=ON -DCINUX_UBSAN=OFF
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
    UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
        cmake --build build-asan --parallel '"${check_jobs}"' --target test_host
' || failed=1

run_segment tsan bash -e -c '
    cmake -S . -B build-tsan -DCINUX_HOST_TSAN=ON -DCINUX_HOST_ASAN=OFF \
        -DCINUX_LOCKDEP=ON -DCINUX_UBSAN=OFF
    TSAN_OPTIONS=halt_on_error=1 \
        cmake --build build-tsan --parallel '"${check_jobs}"' --target test_host
' || failed=1

run_segment checks bash -e -c '
    cmake -S . -B build-checks -DCINUX_UBSAN=ON -DCINUX_LOCKDEP=ON \
        -DCINUX_HOST_ASAN=OFF -DCINUX_HOST_TSAN=OFF
    cmake --build build-checks --parallel '"${check_jobs}"' --target test_host test_kernel boot_image
' || failed=1

for name in default asan tsan checks; do
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
echo "[acceptance] verdict: all four trees green"
