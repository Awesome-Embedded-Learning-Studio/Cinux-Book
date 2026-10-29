#!/usr/bin/env bash
set -uo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/../.."
root=${CINUX_MATRIX_DIR:-build-matrix}
mkdir -p "$root/logs"
failed=0
run_cell() {
    local suite=$1 build_type=$2 mode=$3
    local name="${suite}-${build_type}-${mode}"
    local directory="${root}/${build_type}-${mode}"
    echo "[matrix] RUN $name"
    echo RUNNING >"$root/logs/$name.status"
    if bash scripts/checks/run-cell.sh "$suite" "$build_type" "$mode" "$directory" >"$root/logs/$name.log" 2>&1 &&
        { [[ "$suite" != kernel ]] || python3 scripts/test_station33_ecosystem.py \
            --image "$directory/boot/cinux.img" >>"$root/logs/$name.log" 2>&1; }; then
        echo OK >"$root/logs/$name.status"
        echo "[matrix] PASS $name"
    else
        echo FAILED >"$root/logs/$name.status"
        cat "$root/logs/$name.log"
        failed=1
    fi
}
for mode in asan tsan; do
    run_cell host Release "$mode"
done
for build_type in Debug Release; do
    for mode in none ubsan lockdep; do
        run_cell kernel "$build_type" "$mode"
    done
done
run_cell kernel Release checks
for mode in none checks; do
    run_cell ecosystem Release "$mode"
done
if [[ "$failed" -ne 0 ]]; then
    echo "[matrix] FAILED; logs: $root/logs"
    exit 1
fi
echo "[matrix] PASS: host sanitizers, six kernel cells, production boots, combined checks and both ecosystem cells"
