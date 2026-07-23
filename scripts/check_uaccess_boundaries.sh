#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

fail=0

run_rg() {
    if command -v rg >/dev/null 2>&1; then
        rg -n "$@"
    else
        grep -RInE "$@"
    fi
}

check_empty() {
    local title="$1"
    local pattern="$2"
    shift 2
    local out
    out="$(run_rg "$pattern" "$@" || true)"
    if [[ -n "$out" ]]; then
        echo "[uaccess] ${title}"
        echo "$out"
        fail=1
    fi
}

# User memory must cross the boundary via user_access.hpp.  A local stac/clac
# window in a syscall/proc/driver path can still panic on a bad user page and
# is easy to accidentally leave open across refactors.
check_empty "manual stac/clac outside user_access.hpp" \
    'cinux::arch::(stac|clac)\(' \
    kernel/proc kernel/syscall kernel/drivers

# Known user-pointer-ish names that previously regressed into raw casts.  Casts
# passed directly into copy_to_user/copy_from_user/put_user/get_user are allowed;
# this check catches the two dangerous shapes: direct deref of the cast, or
# aliasing the cast into a typed pointer for a later raw *p access.
check_empty "raw deref of known user pointer names" \
    '\*reinterpret_cast<[^>]+\*>[[:space:]]*\((addr|buf_virt|uaddr|parent_tid|child_tid|frame->rsp|task->clear_child_tid)\)' \
    kernel/proc kernel/syscall kernel/drivers

check_empty "typed alias of known user pointer names" \
    '(auto\*|[A-Za-z0-9_:<>]+\*)[[:space:]]+[A-Za-z0-9_]+[[:space:]]*=[[:space:]]*reinterpret_cast<[^>]+\*>[[:space:]]*\((addr|buf_virt|uaddr|parent_tid|child_tid|frame->rsp|task->clear_child_tid)\)' \
    kernel/proc kernel/syscall kernel/drivers

# Syscall files are the primary user/kernel boundary.  Use a broader name set
# there so new syscalls do not regress just by picking a different argument
# spelling such as addr_virt, optval, or foo_ptr.
syscall_user_names='([A-Za-z0-9_]*(virt|uaddr|uptr|ptr)|addr|buf|optval|optlen_ptr|addrlen_ptr|parent_tid|child_tid|tidptr)'

check_empty "raw deref of syscall user-like names" \
    "\\*reinterpret_cast<[^>]+\\*>[[:space:]]*\\(${syscall_user_names}\\)" \
    kernel/syscall

check_empty "typed alias of syscall user-like names" \
    "(auto\\*|[A-Za-z0-9_:<>]+\\*)[[:space:]]+[A-Za-z0-9_]+[[:space:]]*=[[:space:]]*reinterpret_cast<[^>]+\\*>[[:space:]]*\\(${syscall_user_names}\\)" \
    kernel/syscall

if [[ "$fail" -ne 0 ]]; then
    echo "[uaccess] use copy_to_user/copy_from_user/put_user/get_user instead"
    exit 1
fi

echo "[uaccess] boundary check passed"
