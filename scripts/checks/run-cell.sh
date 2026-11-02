#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/../.."
if [[ $# -ne 4 ]]; then
    echo "usage: $0 <host|kernel|ecosystem|selfhost|native> <Debug|Release> <none|asan|tsan|ubsan|lockdep|checks> <build-dir>" >&2
    exit 2
fi
suite=$1
build_type=$2
mode=$3
build_dir=$4
check_jobs=${CINUX_CHECK_JOBS:-4}
case "$build_type" in Debug|Release) ;; *) echo "invalid build type: $build_type" >&2; exit 2 ;; esac
case "$suite:$mode" in
    host:none|host:asan|host:tsan|kernel:none|kernel:ubsan|kernel:lockdep|kernel:checks|\
    ecosystem:none|ecosystem:checks|selfhost:none|selfhost:checks|native:none|native:checks) ;;
    *) echo "unsupported test cell: $suite/$mode" >&2; exit 2 ;;
esac
if [[ ! "$check_jobs" =~ ^[1-9][0-9]*$ ]]; then
    echo "CINUX_CHECK_JOBS must be a positive integer" >&2
    exit 2
fi
if [[ "$suite" == native && -e "$build_dir/boot/native-build.ext2" ]]; then
    echo "[cell] native requires a fresh build directory; refusing to overwrite a development volume" >&2
    exit 1
fi
if [[ "$suite" != host ]] && [[ ! -c /dev/kvm || ! -r /dev/kvm || ! -w /dev/kvm ]]; then
    echo "[cell] $suite/$build_type/$mode: /dev/kvm must be readable and writable; KVM is required" >&2
    exit 1
fi

flags=(-DCMAKE_BUILD_TYPE="$build_type"
       "-DCMAKE_C_FLAGS_RELEASE=-O2 -DNDEBUG" "-DCMAKE_CXX_FLAGS_RELEASE=-O2 -DNDEBUG"
       -DCINUX_HOST_ASAN=OFF -DCINUX_HOST_TSAN=OFF
       -DCINUX_UBSAN=OFF -DCINUX_LOCKDEP=OFF)
case "$mode" in
    asan) flags+=(-DCINUX_HOST_ASAN=ON -DCINUX_LOCKDEP=ON) ;;
    tsan) flags+=(-DCINUX_HOST_TSAN=ON -DCINUX_LOCKDEP=ON) ;;
    ubsan) flags+=(-DCINUX_UBSAN=ON) ;;
    lockdep) flags+=(-DCINUX_LOCKDEP=ON) ;;
    checks) flags+=(-DCINUX_UBSAN=ON -DCINUX_LOCKDEP=ON) ;;
esac
launcher=${CINUX_COMPILER_LAUNCHER:-}
if [[ "$suite" == selfhost || "$suite" == native ]]; then
    launcher=""
fi
flags+=(-DCMAKE_C_COMPILER_LAUNCHER="$launcher" -DCMAKE_CXX_COMPILER_LAUNCHER="$launcher")
cmake -S . -B "$build_dir" "${flags[@]}"
python3 scripts/checks/test-inventory.py --build-dir "$build_dir"
export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1
export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
export TSAN_OPTIONS=halt_on_error=1
echo "[cell] RUN $suite/$build_type/$mode ($build_dir)"
case "$suite" in
    host)
        cmake --build "$build_dir" --parallel "$check_jobs" --target test_host
        ;;
    kernel)
        cmake --build "$build_dir" --parallel "$check_jobs" --target test_host test_kernel boot_image
        ;;
    ecosystem)
        cmake --build "$build_dir" --parallel "$check_jobs" --target boot_image ext2_img qemu_disks
        python3 scripts/test_station33_ecosystem.py --image "$build_dir/boot/cinux.img"
        python3 scripts/test_station34_selfhost.py --image "$build_dir/boot/cinux.img"
        python3 scripts/test_net_ping.py --image "$build_dir/boot/cinux.img" --log "$build_dir/station31-ping.log"
        python3 scripts/test_net_tcp.py --build-dir "$build_dir" --log-dir "$build_dir/tcp-smoke"
        python3 scripts/test_station35_gui.py --image "$build_dir/boot/cinux.img" --keep
        ;;
    selfhost)
        cmake --build "$build_dir" --parallel "$check_jobs" --target kernel_selfhost_volume
        python3 scripts/test_station34_kernel_selfhost.py --image "$build_dir/boot/cinux.img"
        ;;
    native)
        cmake --build "$build_dir" --parallel "$check_jobs" --target buildroot_rootfs
        python3 scripts/test_native_build.py --image "$build_dir/boot/cinux.img"
        python3 scripts/test_native_gui.py --image "$build_dir/boot/cinux.img"
        python3 scripts/test_persistence.py --image "$build_dir/boot/cinux.img"
        python3 scripts/test_persistence.py --image "$build_dir/boot/cinux.img" --gui
        ;;
esac
echo "[cell] PASS $suite/$build_type/$mode"
