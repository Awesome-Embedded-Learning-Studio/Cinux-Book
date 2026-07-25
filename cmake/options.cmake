# =============================================================================
# cmake/options.cmake
# @brief Single registry of every user-facing Cinux build switch.
#
# Add a new switch HERE, not scattered across CMakeLists.txt files. Switches in
# CINUX_COMPILE_DEF_OPTS below are auto-mapped to a PUBLIC compile definition of
# the same name on big_kernel_common (kernel/CMakeLists.txt foreach) -- so a new
# driver/smoke flag is one option() here + one list entry, nothing else.
#
# Groups:
#   1. Feature drivers      -- hardware subsystems (GUI/USB/NET)
#   2. Debug instrumentation -- opt-in diagnostics (LOCKDEP/UBSAN)
#   3. Ring-3 smoke tests   -- musl/busybox/gcc ecosystem probes
#   4. Host sanitizers      -- test/ only (HOST_ASAN/HOST_TSAN)
#   5. Build switches       -- what gets built (BUILD_TESTS)
# =============================================================================

# ---- 1. Feature drivers -----------------------------------------------------
option(CINUX_GUI "Enable GUI mode (window manager + terminal + host adapter)" ON)
option(CINUX_USB "Enable USB (xHCI host controller + PCI MSI-X) driver" ON)
option(CINUX_NET "Enable e1000 Intel NIC driver + L3/L4 stack" ON)
option(CINUX_VIRTIO "Enable VirtIO PCI modern transport + virtio-blk/net driver" ON)

# ---- 2. Debug instrumentation (opt-in, zero cost when off) ------------------
# F-INFRA I-10: assert no spinlock is held across schedule() (would deadlock
# single-core). Enable for deadlock hunting: cmake -DCINUX_LOCKDEP=ON ...
option(CINUX_LOCKDEP "Enable lockdep schedule-while-locked debug checks" OFF)

# F-INFRA I-9: -fsanitize=undefined instruments the kernel to call freestanding
# __ubsan_handle_* stubs (kernel/lib/ubsan.cpp) that kpanic on UB. Compile-only
# (no libubsan at link -- the stubs resolve the references). Enable for UB
# hunting: cmake -DCINUX_UBSAN=ON ... NOTE: handled separately from the foreach
# below because it also needs set_source_files_properties to exclude the stub +
# diagnostic path (see kernel/CMakeLists.txt).
option(CINUX_UBSAN "Enable -fsanitize=undefined with freestanding UBSan stubs" OFF)

# B1 (gcc-compile-stutter): spawn a resident low-priority kthread that prints
# dump_memory_stats() (PMM free / slab pages / PageCache cached / cumulative
# #PF) to the serial log every ~1 s.  For profiling a workload -- e.g. g++
# hello.cpp -- to read the stutter root cause off the trend instead of guessing
# (page_cache grow-only vs demand-page storm vs slab pressure).  Off by default:
# it adds 1 Hz [MEM] lines to every boot log.  §14 file gate: ON links
# stats_kthread.cpp, OFF links stats_kthread_stub.cpp (empty); no source #ifdef.
option(CINUX_STATS_KTHREAD "Spawn periodic 1 Hz memory-stats kthread for ad-hoc profiling" OFF)

# B3 defect C: spawn the TLB drain kthread (deferred CoW shootdown+free).
# ON (default) links tlb_drain.cpp -- handle_cow_fault defers the free to a
# kthread that shootdowns all cores first.  OFF links tlb_drain_stub.cpp
# (empty start_tlb_drain_thread); g_drain_active stays false so
# enqueue_pending_shootdown inline-frees with no shootdown (defect C stale
# tolerated).  §14 file gate: no source #ifdef.
option(CINUX_TLB_DRAIN "Spawn the TLB shootdown drain kthread (deferred CoW free)" ON)

# F-DYN-COV: SMP data-race watchpoint detector.  RACE_TOUCH(w) marks a shared
# variable that ought to be lock-protected and kpanics if two different CPUs
# touch it with no lock in between -- the signature of a lockless shared-state
# race (e.g. ext2 inode_cache_ before it got a lock).  lockdep_assert_held
# (always available under CINUX_LOCKDEP) catches "designed a lock but forgot
# to take it on some path".  Default OFF: the watchpoint adds an atomic
# exchange per touch and perturbs timing (heisenbug risk); enable for race
# hunting: cmake -DCINUX_RACE_DETECT=ON -DCINUX_LOCKDEP=ON ...
# §14 file gate: ON links race_detect.cpp, OFF links race_detect_stub.cpp.
option(CINUX_RACE_DETECT "Enable SMP data-race watchpoint detector (debug)" OFF)

# ---- 3. Ring-3 smoke tests (run-kernel-test harness; need artifacts) --------
# F10-M1 batch 6 / P3: musl /hello ring-3 smoke -- the ONLY test that exercises
# real user-space syscall paths under SMAP (run-kernel-test uses kernel
# addresses so SMAP never fires there). Requires musl sysroot + /hello +
# /forktest on ext2 (tools/musl/). CI caches the sysroot; ~30s one-time locally.
option(CINUX_MUSL_HELLO_SMOKE "Enable musl /hello ring-3 smoke in run-kernel-test" ON)

# F10-M2: musl DYNAMIC /hello-dyn ring-3 smoke (PT_INTERP / interpreter-load
# path; ldso relocates the program in user space). Requires musl sysroot +
# /hello-dyn + /lib/ld-musl-x86_64.so.1. Default OFF (CI has no sysroot).
option(CINUX_MUSL_DYN_SMOKE "Enable musl dynamic /hello-dyn ring-3 smoke in run-kernel-test" OFF)

# F-GUI-USERSPACE batch 1b: /dev/fb0 mmap ring-3 smoke (IoPhys VMA fault path --
# the only test that triggers the batch-1a device-mmap mechanism). Requires the
# musl sysroot + /fb_mmap_test on ext2 (tools/musl/build-fb-mmap-test.sh; not a
# CMake target). Default OFF (CI has no sysroot).
option(CINUX_FB_MMAP_SMOKE "Enable /dev/fb0 mmap ring-3 smoke in run-kernel-test" OFF)

# F-GUI-USERSPACE batch 2: /dev/event0 input ring-3 smoke (ISR push -> ring ->
# copy_to_user -- the only test that exercises the userspace input device).
# Requires the musl sysroot + /input_event_test on ext2
# (tools/musl/build-input-event-test.sh; not a CMake target). Default OFF.
option(CINUX_INPUT_SMOKE "Enable /dev/event0 input ring-3 smoke in run-kernel-test" OFF)

# F-GUI-USERSPACE batch 3a: userspace GUI host smoke (Cinux-GUI core + Cinux
# host adapter fork+execve'd -- exercises the core compiled into a userspace
# ELF + Host ABI wiring + fb render path). Built by tools/musl/build-cinux-gui-host.sh.
option(CINUX_GUI_HOST_SMOKE "Enable userspace GUI host ring-3 smoke in run-kernel-test" OFF)

# F-ECO: busybox ring-3 ecosystem smoke. fork+execve /bin/busybox to run a
# spread of applets -- the first "run real programs" touchstone. Requires the
# busybox ELF at build/musl/busybox (tools/musl/build-busybox.sh; not a CMake
# target). Default OFF; CI enables it in the dedicated busybox-smoke job.
option(CINUX_BUSYBOX_SMOKE "Enable busybox ring-3 ecosystem smoke in run-kernel-test" OFF)

# B4-B1: optionally stage the host's glibc-dynamic GCC toolchain subset
# (as/ld + glibc runtime + crt + libgcc; no cc1/headers) into the ext2 disk for
# the GCC self-host smoke. Off by default: CI lacks GCC-private crt, and the
# subset is a host artifact. cmake/qemu.cmake acts on this flag (disk size +
# staging); kernel/CMakeLists.txt maps it to a compile definition.
option(CINUX_GCC_TOOLCHAIN "Stage host glibc GCC subset (as/ld+crt+libgcc) into ext2 disk" OFF)

# ---- 4. Host unit test sanitizers (test/ only; zero kernel change) ----------
# F-QA Q1-5: opt-in ASan + UBSan + gcov for host unit tests. Catches UAF/OOB/UB
# at the InodeOps/mock layer. TSan is mutually exclusive (separate flag below).
option(CINUX_HOST_ASAN "Host unit tests: AddressSanitizer + UBSan + gcov coverage" OFF)

# F-VERIFY M4: opt-in ThreadSanitizer -- the data-race detector (lockdep only
# does lock-order). Mutually exclusive with CINUX_HOST_ASAN. Applies to the
# -pthread tests (pmm concurrent, sync_concurrent).
option(CINUX_HOST_TSAN "Host unit tests: ThreadSanitizer (data-race detector for -pthread tests)" OFF)


# ---- 5. Build switches ------------------------------------------------------
# Controls whether test/ (host unit tests + test kernel image) is added. OFF by
# default for zero behaviour change: historically injected via -D from CI
# (.github/workflows/ci.yml), VSCode (.vscode/tasks.json), and scripts; never
# declared, so a bare `cmake -B build -S .` silently skipped test/. Declaring
# it makes it visible in ccmake/cmake-gui without changing the default.
option(CINUX_BUILD_TESTS "Build host unit tests (top-level test/ dir -> test_host target). Test kernels (big_kernel_test + mini_kernel_test) build unconditionally." OFF)

# F5-M3 后(2026-07-06):host /dev/kvm GID 漂到 kmem(用户只在 kvm 组)→ permission
# denied,KVM 不可用。默认 TCG 模拟;KVM 恢复后 -DCINUX_USE_KVM=ON 显式开。
option(CINUX_USE_KVM "Use KVM acceleration (default OFF; TCG until host /dev/kvm GID is fixed)" OFF)

# =============================================================================
# Production rootfs (buildroot base + GCC closure)
# =============================================================================
# The production rootfs is ALWAYS buildroot-based now: assemble-gcc-rootfs packs
# buildroot base + GCC closure + /cinux_gui_host into rootfs-gcc.ext2. `run` (and
# the usability targets) depend on it, so `cmake --build build --target run`
# builds it automatically -- no manual assemble step.
# (The handcrafted ext2.img stays only as the run-kernel-test-all TEST disk --
# EXT2_IMAGE in cmake/qemu.cmake -- a separate concern, not a user-facing rootfs
# profile anymore.)
set(CINUX_ROOTFS_BUILDROOT_IMG "${CMAKE_BINARY_DIR}/rootfs-gcc.ext2"
    CACHE FILEPATH "Production rootfs.ext2 (buildroot + GCC closure, assemble-gcc-rootfs output)")

# =============================================================================
# Switches that map 1:1 to a same-named PUBLIC compile definition on
# big_kernel_common. kernel/CMakeLists.txt loops over this list instead of one
# if() per flag. CINUX_UBSAN is intentionally absent (see comment above).
# =============================================================================
set(CINUX_COMPILE_DEF_OPTS
    GUI USB NET VIRTIO LOCKDEP RACE_DETECT
    MUSL_HELLO_SMOKE MUSL_DYN_SMOKE BUSYBOX_SMOKE GCC_TOOLCHAIN FB_MMAP_SMOKE
    INPUT_SMOKE
    GUI_HOST_SMOKE)
