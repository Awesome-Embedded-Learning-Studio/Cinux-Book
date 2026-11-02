#!/usr/bin/env bash
set -u

if [ ! -f build/compile_commands.json ]; then
    echo "[clang-tidy] build/compile_commands.json not found (failing, not skipping)"
    echo "[clang-tidy] configure the build first: cmake -S . -B build"
    exit 1
fi

# Adapt GCC-only frontend flags in the temporary AST database. Real tool
# failures remain fatal even when no diagnostic names a project file.
repository="$(pwd -P)"
has_project_diagnostics() {
    grep -F "${repository}/" "$report" | grep -E ': (error|warning):'
}
mapfile -t inputs < <(python3 scripts/hooks/tidy_inputs.py "$@")
if [ "${#inputs[@]}" -eq 0 ]; then
    echo "[clang-tidy] no checkable inputs (refusing)"
    exit 1
fi

tidy_db="$(mktemp -d /tmp/cinux-tidy-db.XXXXXX)"
trap 'rm -rf "$tidy_db"' EXIT
python3 scripts/hooks/tidy_database.py build/compile_commands.json "$tidy_db/compile_commands.json" || exit 1
report="$tidy_db/diagnostics.txt"
if clang-tidy --quiet -p "$tidy_db" "${inputs[@]}" >"$report" 2>"$tidy_db/errors.txt"; then
    if has_project_diagnostics; then
        exit 1
    fi
else
    status=$?
    if has_project_diagnostics; then
        exit "$status"
    fi
    echo "[clang-tidy] clang-tidy failed (exit $status) without project diagnostics:"
    cat "$report" "$tidy_db/errors.txt" >&2
    exit "$status"
fi
exit 0
