#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/../.."
check_jobs="${CINUX_CHECK_JOBS:-4}"

cmake -S . -B build -DCINUX_HOST_ASAN=OFF -DCINUX_HOST_TSAN=OFF \
    -DCINUX_LOCKDEP=OFF -DCINUX_UBSAN=OFF
cmake --build build --parallel "$check_jobs" --target test_host test_kernel boot_image

cmake -S . -B build-asan -DCINUX_HOST_ASAN=ON -DCINUX_HOST_TSAN=OFF \
    -DCINUX_LOCKDEP=ON -DCINUX_UBSAN=OFF
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
    cmake --build build-asan --parallel "$check_jobs" --target test_host

cmake -S . -B build-tsan -DCINUX_HOST_TSAN=ON -DCINUX_HOST_ASAN=OFF \
    -DCINUX_LOCKDEP=ON -DCINUX_UBSAN=OFF
TSAN_OPTIONS=halt_on_error=1 \
    cmake --build build-tsan --parallel "$check_jobs" --target test_host

cmake -S . -B build-checks -DCINUX_UBSAN=ON -DCINUX_LOCKDEP=ON \
    -DCINUX_HOST_ASAN=OFF -DCINUX_HOST_TSAN=OFF
cmake --build build-checks --parallel "$check_jobs" --target test_host test_kernel boot_image
