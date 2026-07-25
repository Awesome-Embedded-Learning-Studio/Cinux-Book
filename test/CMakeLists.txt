# =============================================================================
# test/CMakeLists.txt
# @brief Cinux host-side unit test configuration
#
# Defines host-side tests that run on Linux with -DCINUX_HOST_TEST.
# Tests are registered via add_cinux_test() (simple) or
# add_cinux_integration_test() (links kernel sources).
# =============================================================================

set(CINUX_HOST_TEST ON)

# F-QA Q1-2: ErrorOr is [[nodiscard]] (Cinux-Base expected.hpp). Host test
# fixtures ignore some setup ErrorOr returns (e.g. ext2->mount() / pipe write);
# suppress -Wunused-result host-side. DEBT-016 tracks cleaning these (via a
# non-void-safe ASSERT_OK macro) so this directory-level opt-out is removed.
# add_compile_options(-Wno-unused-result)  # F-CLN 批2 removed (ASSERT_OK replaces ignores)

# F-QA Q1-5: opt-in AddressSanitizer + UBSan + gcov coverage for host unit
# tests. Zero kernel changes — only host test targets. Catches UAF/OOB/UB at
# the InodeOps/mock layer; --coverage gives gcov compile+link in one flag.
# TSan is mutually exclusive with ASan, so it is a separate follow-up
# (-DCINUX_HOST_TSAN build) rather than combined here. (option() in
# cmake/options.cmake.)
if(CINUX_HOST_ASAN)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g --coverage)
    add_link_options(-fsanitize=address,undefined --coverage)
endif()

# F-VERIFY M4: opt-in ThreadSanitizer for host unit tests -- the data-race
# detector the project was missing (lockdep only does lock-order; it cannot see
# two cores/threads touching a field with no lock).  Mutually exclusive with
# CINUX_HOST_ASAN (TSan+ASan can't combine), race-detection only (no UBSan/
# coverage -- pair with CINUX_HOST_ASAN for those).  Applies to the -pthread
# tests (test_pmm concurrent alloc/free, test_sync_concurrent 8-thread lock
# stress).  Value ramps once M2 links REAL kernel code (sync/scheduler/PMM) into
# these tests; today it guards the concurrent mirror re-implementations + any new
# -pthread test.  GCC -fsanitize=thread.  (option() in cmake/options.cmake.)
if(CINUX_HOST_TSAN)
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=thread)
endif()

# Host tests need standard library; override kernel toolchain linker flags
set(CMAKE_EXE_LINKER_FLAGS "")

set(TEST_INCLUDE_DIRS
    ${CMAKE_SOURCE_DIR}/test/framework
    ${CMAKE_SOURCE_DIR}/kernel
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/third_party/Cinux-Base/include
)

enable_testing()

# ============================================================
# Helper functions
# ============================================================

# Simple test: compiles unit/test_<name>.cpp with no extra sources.
function(add_cinux_test name)
    add_executable(test_${name} unit/test_${name}.cpp ${ARGN})
    target_compile_definitions(test_${name} PRIVATE CINUX_HOST_TEST)
    target_include_directories(test_${name} PRIVATE ${TEST_INCLUDE_DIRS})
    add_test(NAME ${name} COMMAND test_${name})
    set_tests_properties(${name} PROPERTIES LABELS "${name}")
    # Auto-register: every test built via this helper joins ALL_HOST_TESTS so the
    # test_host / test_all / test_verbose aggregates build+run it. Replaces a
    # hand-maintained list that silently dropped tests added later (net L0/L1,
    # extable, pty, tty, ...). PARENT_SCOPE accumulates across calls.
    list(APPEND ALL_HOST_TESTS test_${name})
    set(ALL_HOST_TESTS ${ALL_HOST_TESTS} PARENT_SCOPE)
endfunction()

# Integration test: arbitrary sources listed after the name.
function(add_cinux_integration_test name)
    add_executable(test_${name} ${ARGN})
    target_compile_definitions(test_${name} PRIVATE CINUX_HOST_TEST)
    target_include_directories(test_${name} PRIVATE
        ${CMAKE_SOURCE_DIR}/test/framework
        ${CMAKE_SOURCE_DIR}/kernel
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/third_party/Cinux-Base/include
    )
    add_test(NAME ${name} COMMAND test_${name})
    set_tests_properties(${name} PROPERTIES LABELS "${name}")
    list(APPEND ALL_HOST_TESTS test_${name})
    set(ALL_HOST_TESTS ${ALL_HOST_TESTS} PARENT_SCOPE)
endfunction()

# ============================================================
# Simple tests (pure arithmetic, no kernel code linked)
# ============================================================

add_cinux_test(smoke)
add_cinux_test(gdt_idt)
add_cinux_test(ata)
add_cinux_test(elf_loader)
add_cinux_test(big_kernel_loader)
add_cinux_test(pic)
add_cinux_test(pit)
# F5-M4 B2: HPET ticks->ns overflow-safe arithmetic (pure, header-only inline).
add_cinux_test(hpet_math)
# F5-M4 B3: RTC BCD decode + Gregorian->Unix-seconds (pure, header-only inline).
add_cinux_test(rtc_math)
add_cinux_test(font)
add_cinux_test(console)
add_cinux_test(framebuffer)
add_cinux_test(keyboard)
# F10-M3 batch 1: TTY line discipline -- links the real drivers/tty/tty.cpp
# (pure logic, zero kprintf) so the canonical discipline is exercised on host.
add_cinux_test(tty ${CMAKE_SOURCE_DIR}/kernel/drivers/tty/tty.cpp)
# F10-M3 Phase 2 batch 1: PTY pair -- links the real pty.cpp + its tty.cpp
# dependency so master/slave round-trip, echo, signal, raw, EOF run on host.
add_cinux_test(pty ${CMAKE_SOURCE_DIR}/kernel/drivers/tty/pty.cpp
                   ${CMAKE_SOURCE_DIR}/kernel/drivers/tty/tty.cpp)
add_cinux_test(extable)  # F-EXTABLE: exception-table search/sort primitives
add_cinux_test(address_space)
add_cinux_test(scheduler)
add_cinux_test(sync)
add_cinux_test(usermode)
add_cinux_test(syscall)
add_cinux_test(shell)
add_cinux_test(ahci)
add_cinux_test(pci)
add_cinux_test(initial_stack)
# xHCI: register layout is header-only; Batch 2A links the real ring-math
# (xhci_ring.cpp) so host tests exercise the actual enqueue/dequeue cycle logic.
add_cinux_integration_test(xhci
    unit/test_xhci.cpp
    ${CMAKE_SOURCE_DIR}/kernel/drivers/usb/xhci_ring.cpp
)
# F5-M5 Batch 3A: USB SETUP packet + descriptor encoding (header-only, pure).
add_cinux_test(usb)
# F5-M5 Batch 4A: HID boot-mouse decode + config-descriptor walk (header-only).
add_cinux_test(hid)
add_cinux_test(ext2)
add_cinux_test(ahci_write)
add_cinux_test(ext2_allocator)
add_cinux_test(ext2_ops)
add_cinux_test(syscall_ext2)
add_cinux_test(shell_write)
add_cinux_test(cwd_stat)
add_cinux_test(canvas)
add_cinux_test(mouse)
add_cinux_test(event_queue)

# Cinux-Base leaf types (header-only) — proves the submodule is usable from the
# host test build before any kernel migration relies on it. Only the include
# path is added here so Cinux-Base's compile flags never leak into other tests.
add_cinux_test(cinux_base_types)
target_include_directories(test_cinux_base_types PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/Cinux-Base/include
)

# F-QA Q4a-1: RefCount (saturating atomic refcount, Linux refcount_t semantics).
# Header-only Cinux-Base type. The submodule's own test/ is never built by the
# main repo (third_party/CMakeLists.txt does not add_subdirectory(Cinux-Base)),
# so coverage must live here; add_cinux_test already adds the Cinux-Base include
# path via TEST_INCLUDE_DIRS.
add_cinux_test(refcount)

# F7 net stack (L0 foundation): NetStack dispatch + L2 parse + buffer recycle
# contract + ARP cache + the Cinux-Base internet checksum. The stack core is
# pure logic (zero kprintf), so net_dispatch links the real net_stack.cpp and
# net_checksum links Cinux-Base/src/checksum.cpp directly -- proving the L3
# stack is host-buildable and driver-agnostic before any NIC is attached.
add_cinux_integration_test(net_dispatch
    unit/test_net_dispatch.cpp
    ${CMAKE_SOURCE_DIR}/kernel/net/net_stack.cpp
)
add_cinux_test(net_arp_cache)
add_cinux_test(net_buffer)
add_cinux_integration_test(net_checksum
    unit/test_net_checksum.cpp
    ${CMAKE_SOURCE_DIR}/third_party/Cinux-Base/src/checksum.cpp
)
# F7 L1: ArpModule (request->reply, reply->cache, resolve_l3 miss->request).  Mock
# Ethernet device; links the real arp.cpp + net_stack.cpp.
add_cinux_integration_test(net_arp_module
    unit/test_net_arp_module.cpp
    ${CMAKE_SOURCE_DIR}/kernel/net/arp.cpp
    ${CMAKE_SOURCE_DIR}/kernel/net/net_stack.cpp
)
# F7 M4: UdpModule (L4Handler proto 17) + the Ipv4Module inner L4 table it rides
# on.  A no-L2 mock device gives a send -> IPv4 table -> handle -> listener
# round-trip on host; links the real udp/ipv4/icmp/net_stack + Cinux-Base checksum.
add_cinux_integration_test(net_udp
    unit/test_net_udp.cpp
    ${CMAKE_SOURCE_DIR}/kernel/net/udp.cpp
    ${CMAKE_SOURCE_DIR}/kernel/net/ipv4.cpp
    ${CMAKE_SOURCE_DIR}/kernel/net/icmp.cpp
    ${CMAKE_SOURCE_DIR}/kernel/net/arp.cpp
    ${CMAKE_SOURCE_DIR}/kernel/net/net_stack.cpp
    ${CMAKE_SOURCE_DIR}/third_party/Cinux-Base/src/checksum.cpp
)

# F7-M5 batch 1: TCP wire layer + the inbound pseudo-header checksum gate.
# A no-L2 mock feeds a hand-built TCP segment over IPv4 through the real L4
# dispatch; links the real tcp/ipv4/icmp/arp/net_stack + Cinux-Base checksum.
add_cinux_integration_test(net_tcp
    unit/test_net_tcp.cpp
    ${CMAKE_SOURCE_DIR}/kernel/net/tcp.cpp
    ${CMAKE_SOURCE_DIR}/kernel/net/ipv4.cpp
    ${CMAKE_SOURCE_DIR}/kernel/net/icmp.cpp
    ${CMAKE_SOURCE_DIR}/kernel/net/arp.cpp
    ${CMAKE_SOURCE_DIR}/kernel/net/net_stack.cpp
    ${CMAKE_SOURCE_DIR}/third_party/Cinux-Base/src/checksum.cpp
)

# ============================================================
# Tests with threading support
# ============================================================

add_cinux_test(pmm)
target_link_libraries(test_pmm PRIVATE -pthread)

add_cinux_test(sync_concurrent)
target_link_libraries(test_sync_concurrent PRIVATE -pthread)

add_cinux_test(kprintf)

# ============================================================
# Tests with extra kernel sources
# ============================================================

add_cinux_integration_test(kprintf_format
    unit/test_kprintf_format.cpp
    ${CMAKE_SOURCE_DIR}/kernel/mini/lib/private/format.cpp
)

# F5-M5 Batch 0A: MSI-X capability discovery (pure, reader-injected). Links
# the real kernel/drivers/pci/msix.cpp; the test supplies a mock config-space
# reader so no real 0xCF8/0xCFC I/O is needed.
add_cinux_integration_test(msix
    unit/test_msix.cpp
    ${CMAKE_SOURCE_DIR}/kernel/drivers/pci/msix.cpp
)

add_cinux_integration_test(vmm
    unit/test_vmm.cpp
)

add_cinux_integration_test(ramdisk
    unit/test_ramdisk.cpp
)

add_cinux_integration_test(fd_table
    unit/test_fd_table.cpp
    unit/host_spinlock.cpp
    ${CMAKE_SOURCE_DIR}/kernel/fs/inode.cpp
    ${CMAKE_SOURCE_DIR}/kernel/fs/file.cpp
)

add_cinux_integration_test(vfs_mount
    unit/test_vfs_mount.cpp
    unit/host_spinlock.cpp
    ${CMAKE_SOURCE_DIR}/kernel/fs/inode.cpp
    ${CMAKE_SOURCE_DIR}/kernel/fs/vfs_mount.cpp
    ${CMAKE_SOURCE_DIR}/kernel/fs/file.cpp
)

# F6-M3 DevFS: links the real kernel/fs/devfs/devfs.cpp (pure logic, zero kprintf)
# plus inode.cpp (the InodeOps default methods the device vtables reference).
# The console device uses a capturing mock sink (no real serial dependency).
add_cinux_integration_test(devfs
    unit/test_devfs.cpp
    unit/host_spinlock.cpp
    ${CMAKE_SOURCE_DIR}/kernel/fs/inode.cpp
    ${CMAKE_SOURCE_DIR}/kernel/fs/devfs/devfs.cpp
    ${CMAKE_SOURCE_DIR}/kernel/fs/file.cpp  # inode_ref/unref (devfs lookup returns ref'd inodes)
)

add_cinux_integration_test(ext2_inode_ops
    unit/test_ext2_inode_ops.cpp
    ${CMAKE_SOURCE_DIR}/kernel/fs/inode.cpp
)

# F6-M6: ext2 host-side build. The driver is compiled directly into each host
# integration test (net_tcp pattern -- no STATIC lib: the kernel build needs
# -mcmodel=kernel codegen, the host build must not, so one target cannot serve
# both). EXT2_HOST_LIB_SOURCES is the shared link list: the real ext2 sources
# + the host PAL (kprintf/kmalloc) + Spinlock + inode_ref/unref.
set(EXT2_HOST_LIB_SOURCES
    unit/ext2_host_pal.cpp
    unit/host_spinlock.cpp
    ${CMAKE_SOURCE_DIR}/kernel/fs/file.cpp
    ${CMAKE_SOURCE_DIR}/kernel/fs/inode.cpp
    ${CMAKE_SOURCE_DIR}/libs/ext2/ext2_common.cpp
    ${CMAKE_SOURCE_DIR}/libs/ext2/ext2_dirops.cpp
    ${CMAKE_SOURCE_DIR}/libs/ext2/ext2_init.cpp
    ${CMAKE_SOURCE_DIR}/libs/ext2/ext2_inode.cpp
    ${CMAKE_SOURCE_DIR}/libs/ext2/ext2_block.cpp
    ${CMAKE_SOURCE_DIR}/libs/ext2/ext2_directory.cpp
    ${CMAKE_SOURCE_DIR}/libs/ext2/ext2_links.cpp
    ${CMAKE_SOURCE_DIR}/libs/ext2/ext2_metadata.cpp
    ${CMAKE_SOURCE_DIR}/libs/ext2/ext2_extent.cpp
)

# B1a: PAL link smoke. ctor + mount (zeroed image -> magic fail) + dtor. Gates
# that the host PAL coverage is complete (the M6 make-or-break link).
add_cinux_integration_test(ext2_host_link
    unit/test_ext2_host_link.cpp
    ${EXT2_HOST_LIB_SOURCES}
)

# B1b: real-logic ASAN test. Mounts the prebuilt 64 KiB image and exercises
# readdir / multi-level lookup / read / create / write / read-back / unlink
# through the real VFS path (the host counterpart of the F-DYN-COV QEMU
# forensics -- races / UAF surface in milliseconds under ASAN/TSAN).
add_cinux_integration_test(ext2_host
    unit/test_ext2_host.cpp
    ${EXT2_HOST_LIB_SOURCES}
)
target_compile_definitions(test_ext2_host PRIVATE
    EXT2_TEST_IMG_PATH="${CMAKE_SOURCE_DIR}/test/data/ext2_test.img")

# B2: host TSAN concurrency test. N threads over one Ext2 stress the
# block_alloc_lock_ (alloc/free) and inode_cache_lock_ (lookup); TSan sees
# races lockdep cannot -- the host counterpart of the F-DYN-COV QEMU forensics.
# Build with -DCINUX_HOST_TSAN=ON (mutually exclusive with ASAN).
add_cinux_integration_test(ext2_concurrent
    unit/test_ext2_concurrent.cpp
    ${EXT2_HOST_LIB_SOURCES}
)
target_compile_definitions(test_ext2_concurrent PRIVATE
    EXT2_TEST_IMG_PATH="${CMAKE_SOURCE_DIR}/test/data/ext2_test.img")
target_link_libraries(test_ext2_concurrent PRIVATE -pthread)

add_cinux_integration_test(pipe
    unit/test_pipe.cpp
    unit/host_spinlock.cpp
    ${CMAKE_SOURCE_DIR}/kernel/ipc/pipe.cpp
    ${CMAKE_SOURCE_DIR}/kernel/ipc/pipe_ops.cpp
    ${CMAKE_SOURCE_DIR}/kernel/fs/inode.cpp
)

add_cinux_integration_test(sys_pipe
    unit/test_sys_pipe.cpp
    unit/host_spinlock.cpp
    ${CMAKE_SOURCE_DIR}/kernel/fs/inode.cpp
    ${CMAKE_SOURCE_DIR}/kernel/fs/file.cpp
    ${CMAKE_SOURCE_DIR}/kernel/ipc/pipe.cpp
    ${CMAKE_SOURCE_DIR}/kernel/ipc/pipe_ops.cpp
)

add_cinux_integration_test(shell_redirect
    unit/test_shell_redirect.cpp
    unit/host_spinlock.cpp
    ${CMAKE_SOURCE_DIR}/kernel/fs/inode.cpp
    ${CMAKE_SOURCE_DIR}/kernel/fs/file.cpp
    ${CMAKE_SOURCE_DIR}/kernel/fs/vfs_mount.cpp
    ${CMAKE_SOURCE_DIR}/kernel/ipc/pipe.cpp
    ${CMAKE_SOURCE_DIR}/kernel/ipc/pipe_ops.cpp
)

add_cinux_integration_test(fork_exec
    unit/test_fork_exec.cpp
    ${CMAKE_SOURCE_DIR}/kernel/proc/pid.cpp
    unit/host_spinlock.cpp
    ${CMAKE_SOURCE_DIR}/kernel/proc/elf_types.cpp
)

# F8-M2 batch 3: named FIFO -- links the real fifo.cpp + pipe.cpp + pipe_ops.cpp
# (pure logic, host-safe blocking) so FifoRegistry + FifoOps cloning open run on
# host.
add_cinux_integration_test(fifo
    unit/test_fifo.cpp
    unit/host_spinlock.cpp
    ${CMAKE_SOURCE_DIR}/kernel/ipc/fifo.cpp
    ${CMAKE_SOURCE_DIR}/kernel/ipc/pipe.cpp
    ${CMAKE_SOURCE_DIR}/kernel/ipc/pipe_ops.cpp
    ${CMAKE_SOURCE_DIR}/kernel/fs/inode.cpp
)

# ============================================================
# Custom test targets
# ============================================================

# ALL_HOST_TESTS is auto-populated by add_cinux_test / add_cinux_integration_test
# above (every registered host test appends itself). The aggregates below just
# depend on it -- no test ever needs to be listed by hand, so new tests cannot
# silently go un-built (the old hand-maintained list dropped net L0/L1, extable,
# pty, tty, initial_stack).

add_custom_target(test_host
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    DEPENDS ${ALL_HOST_TESTS}
    USES_TERMINAL
    COMMENT "Running host unit tests..."
)

configure_file(
    ${CMAKE_SOURCE_DIR}/scripts/run_all_tests.sh.in
    ${CMAKE_BINARY_DIR}/run_all_tests.sh
    @ONLY
)

add_custom_target(test_all
    COMMAND sh ${CMAKE_BINARY_DIR}/run_all_tests.sh
    DEPENDS ${ALL_HOST_TESTS} test-image
    USES_TERMINAL
    COMMENT "Running ALL tests (host then kernel)..."
)

add_custom_target(test_verbose
    COMMAND ${CMAKE_CTEST_COMMAND} --verbose --output-on-failure
    DEPENDS ${ALL_HOST_TESTS}
    COMMENT "Running host tests in verbose mode..."
)

add_custom_target(test_smoke_run
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -R smoke
    DEPENDS test_smoke
    COMMENT "Running smoke test..."
)
