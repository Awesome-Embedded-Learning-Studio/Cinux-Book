#
# kernel/CMakeLists.txt
#
# @brief Cinux kernel module CMake configuration
#
# Manages both the mini kernel (bootstrap) and the big kernel.
# Big kernel sources are organised into subdirectory CMakeLists.txt
# files that add sources via target_sources().
#

# ============================================================
# Mini Kernel (Bootstrap Kernel)
# ============================================================
add_subdirectory(mini)

# ============================================================
# Big Kernel - Full-featured kernel loaded by mini kernel
# ============================================================

# Compile options for the big kernel
set(BIG_KERNEL_COMPILE_OPTIONS
    -ffreestanding
    -fno-exceptions
    -fno-rtti
    -fno-pie
    -fno-stack-protector
    -mcmodel=kernel
    -mno-red-zone
    -Wall
    -Wextra
    # F-INFRA I-3 (R2): bring the kernel body up to the Cinux-Base submodule's
    # warning discipline (the submodule already enforces -Wshadow/-Wold-style-cast
    #/-Wnon-virtual-dtor). -Wconversion/-Wpedantic stay OUT here (kernel uses GNU
    # inline-asm extensions; -Wconversion is too noisy — see follow-up G6).
    # -Werror=return-type is a near-zero-noise correctness check (already clean).
    # (-Werror=implicit-int / implicit-function-declaration are C-only and emit
    # "not valid for C++" noise in a C++ kernel, so omitted.)
    -Wshadow
    -Wold-style-cast
    -Wnon-virtual-dtor
    -Woverloaded-virtual
    -Wformat=2
    -Werror=return-type
    # F-QA Q1-1: zero-cost warning gates. Enable the checks, then escalate the
    # zero-noise ones to -Werror= so violations fail the build (zero-warning
    # discipline, aligned with Linux's -Wimplicit-fallthrough / CONFIG_W* ethos).
    # -Wnull-dereference stays a plain warning (GCC cross-function analysis
    # occasionally false-positives). -Wframe-larger-than is DEFERRED: 9 syscall
    # handlers currently put char[PATH_MAX] buffers on the 16 KB stack (4-8 KB
    # frames, DEBT-015) — enabling it now would break the zero-warning build,
    # so it lands in a dedicated stack-safety batch once those frames shrink.
    -Wvla
    -Wimplicit-fallthrough=3
    -Wundef
    -Wduplicated-branches
    -Wduplicated-cond
    -Wlogical-op
    -Wnull-dereference
    -Wformat-security
    -Werror=vla
    -Werror=implicit-fallthrough
    -Werror=undef
    -Werror=duplicated-branches
    -Werror=duplicated-cond
    -Werror=logical-op
    -Werror=format-security
)

set(BIG_KERNEL_LINK_OPTIONS
    -T ${CMAKE_CURRENT_SOURCE_DIR}/linker.ld
    -nostdlib
    -static
    -no-pie
    # The custom linker.ld discards .note.gnu.build-id, so GCC's default
    # --build-id is ignored and ld warns. A build-id is meaningless for a kernel
    # ELF loaded at a fixed physical address; disable it explicitly for a clean
    # link (F-INFRA I-3: zero-warning build).
    -Wl,--build-id=none
)

# Object library: shared sources between production and test kernels.
# Subdirectory CMakeLists.txt files populate this via target_sources().
add_library(big_kernel_common OBJECT)

target_compile_options(big_kernel_common PUBLIC ${BIG_KERNEL_COMPILE_OPTIONS})

target_include_directories(big_kernel_common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}
)

# Third-party libraries (Cinux-Base etc.)
add_subdirectory(${CMAKE_SOURCE_DIR}/third_party ${CMAKE_BINARY_DIR}/third_party)

# Sub-modules (each adds its sources to big_kernel_common)
add_subdirectory(arch)
add_subdirectory(drivers)
add_subdirectory(fs)
add_subdirectory(lib)
add_subdirectory(mm)
add_subdirectory(proc)
add_subdirectory(syscall)
add_subdirectory(ipc)

# GUI mode: add compile definition and GUI-specific sources
if(CINUX_GUI)
    target_compile_definitions(big_kernel_common PUBLIC CINUX_GUI)
    add_subdirectory(gui)
endif()

# F-INFRA I-10: opt-in lockdep checks (assert no spinlock held across schedule()).
if(CINUX_LOCKDEP)
    target_compile_definitions(big_kernel_common PUBLIC CINUX_LOCKDEP)
endif()

# F-INFRA I-9: opt-in UBSan. -fsanitize=undefined at COMPILE only (the freestanding
# stubs in kernel/lib/ubsan.cpp resolve the __ubsan_handle_* refs -- no libubsan).
# Exclude the stub + the diagnostic/panic path so a UB hit cannot recurse.
if(CINUX_UBSAN)
    target_compile_options(big_kernel_common PUBLIC -fsanitize=undefined -fno-sanitize=alignment)
    set_source_files_properties(
        ${CMAKE_SOURCE_DIR}/kernel/lib/ubsan.cpp
        ${CMAKE_SOURCE_DIR}/kernel/lib/kprintf.cpp
        ${CMAKE_SOURCE_DIR}/kernel/arch/x86_64/backtrace.cpp
        ${CMAKE_SOURCE_DIR}/kernel/mm/diagnostics.cpp
        ${CMAKE_SOURCE_DIR}/kernel/arch/x86_64/exception_handlers.cpp
        PROPERTIES COMPILE_OPTIONS "-fno-sanitize=undefined")
endif()

# ============================================================
# Initrd (embedded ustar archive)
# ============================================================

# Convert the initrd tar file into a linkable ELF object.
# This produces symbols _binary_initrd_start / _binary_initrd_end / _binary_initrd_size.
set(INITRD_TAR ${CMAKE_CURRENT_SOURCE_DIR}/data/initrd.tar)
set(INITRD_OBJ  ${CMAKE_BINARY_DIR}/kernel/initrd.o)

# Convert initrd.tar into a linkable ELF object with stable symbol names
# (_binary_initrd_start / _binary_initrd_end / _binary_initrd_size).
# Uses a helper script because objcopy derives symbol names from the
# absolute input path, making them non-portable.
set(EMBED_SCRIPT ${CMAKE_SOURCE_DIR}/scripts/embed_binary.sh)

add_custom_command(
    OUTPUT  ${INITRD_OBJ}
    COMMAND bash ${EMBED_SCRIPT}
        ${INITRD_TAR} ${INITRD_OBJ} .initrd _binary_initrd
    DEPENDS ${INITRD_TAR} ${EMBED_SCRIPT}
    COMMENT "Converting initrd.tar -> initrd.o (embedded ramdisk)"
)

add_custom_target(initrd_obj DEPENDS ${INITRD_OBJ})

# Production big kernel executable
add_executable(big_kernel
    main.cpp
)

target_sources(big_kernel PRIVATE $<TARGET_OBJECTS:big_kernel_common>)
target_link_libraries(big_kernel PRIVATE big_kernel_common)
# F13 cinux::gui: link the host-neutral core (third_party/Cinux-GUI/, GUI only).
if(CINUX_GUI)
    target_link_libraries(big_kernel PRIVATE cinux-gui)
endif()
add_dependencies(big_kernel user_binary_obj initrd_obj)
target_sources(big_kernel PRIVATE
    ${CMAKE_BINARY_DIR}/user/user_binary.o
    ${INITRD_OBJ}
)

set_target_properties(big_kernel PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/kernel/big
    OUTPUT_NAME "big_kernel"
)

target_link_options(big_kernel PRIVATE ${BIG_KERNEL_LINK_OPTIONS})

# ============================================================
# Big Kernel Test Binary (QEMU in-kernel tests)
# ============================================================
# Separate executable that replaces kernel_main with a test harness.
# Gets loaded by the mini kernel just like the production big kernel.

add_executable(big_kernel_test
    test/main_test.cpp
    test/test_gdt_idt.cpp
    test/test_pic_pit.cpp
    test/test_acpi.cpp
    test/test_apic.cpp
    test/test_video.cpp
    test/test_keyboard.cpp
    test/test_pmm.cpp
    test/test_vmm.cpp
    test/test_address_space.cpp
    test/test_scheduler.cpp
    test/test_sync.cpp
    test/test_sync_concurrent.cpp
    test/test_usermode.cpp
    test/test_syscall.cpp
    test/test_shell.cpp
    test/test_ahci.cpp
    test/test_ramdisk.cpp
    test/test_vfs_syscall.cpp
    test/test_ext2.cpp
    test/test_ahci_write.cpp
    test/test_ahci_block_device.cpp
    test/test_ext2_allocator.cpp
    test/test_ext2_ops.cpp
    test/test_ext2_inode_ops.cpp
    test/test_syscall_ext2.cpp
    test/test_shell_write.cpp
    test/test_cwd_stat.cpp
    test/test_canvas.cpp
    test/test_mouse_event.cpp
    test/test_window.cpp
    test/test_window_manager.cpp
    test/test_gui_integration.cpp
    test/test_bitmap_icon.cpp
    test/test_desktop.cpp
    test/test_terminal.cpp
    test/test_pipe.cpp
    test/test_sys_pipe.cpp
    test/test_terminal_shell.cpp
    test/test_fork_exec.cpp
    test/test_multi_terminal.cpp
    test/test_kprintf_format.cpp
    test/test_concurrent_ring_buffer.cpp
    test/test_user_ptr.cpp
    test/test_pmm_mapcount.cpp
    test/test_klog.cpp
    test/test_sys_dmesg.cpp
    test/test_dma_buffer.cpp
    test/test_dma_pool.cpp
    test/test_prdt_builder.cpp
    test/test_block_device.cpp
    test/test_vma.cpp
    test/test_mmap.cpp
    test/test_brk.cpp
    test/test_signal.cpp
    test/test_tls.cpp
    test/test_futex.cpp
    test/test_shared_resources.cpp
    test/test_clone.cpp
    test/test_process_group.cpp
    test/test_page_cache.cpp
    test/test_file_mmap.cpp
    test/test_buddy.cpp
    test/test_slab.cpp
    test/test_kmalloc.cpp
    test/test_kallsyms.cpp
    test/test_backtrace.cpp
    test/test_memory_stats.cpp
    test/test_gui_swraster.cpp
    test/test_gui_region.cpp
    test/test_gui_dirty.cpp
)

target_sources(big_kernel_test PRIVATE $<TARGET_OBJECTS:big_kernel_common>)
target_link_libraries(big_kernel_test PRIVATE big_kernel_common)
if(CINUX_GUI)
    target_link_libraries(big_kernel_test PRIVATE cinux-gui)
endif()
add_dependencies(big_kernel_test user_binary_obj initrd_obj)
target_sources(big_kernel_test PRIVATE
    ${CMAKE_BINARY_DIR}/user/user_binary.o
    ${INITRD_OBJ}
)

set_target_properties(big_kernel_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/kernel/big
    OUTPUT_NAME "big_kernel_test"
)

target_link_options(big_kernel_test PRIVATE ${BIG_KERNEL_LINK_OPTIONS})

# F-QA Q1-2: ErrorOr is [[nodiscard]] (Cinux-Base expected.hpp), so ignoring an
# ErrorOr return warns under -Wunused-result. Production kernel code is clean
# (zero ignores); kernel/test/ fixtures deliberately ignore some setup returns
# (e.g. ramdisk/ext2 mount()) inside non-void helpers where TEST_ASSERT_TRUE's
# bare `return;` won't compile. Suppress in the test binary only — DEBT-016
# tracks replacing these with a non-void-safe ASSERT_OK macro so this is removed.
target_compile_options(big_kernel_test PRIVATE -Wno-unused-result)

# Build test disk image: copy the test ELF to sectors where the big kernel lives
set(BIG_KERNEL_TEST_BIN "${CMAKE_BINARY_DIR}/kernel/big/big_kernel_test_crc.bin")

add_custom_command(TARGET big_kernel_test POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        $<TARGET_FILE:big_kernel_test> ${BIG_KERNEL_TEST_BIN}
    COMMAND python3 ${CMAKE_SOURCE_DIR}/scripts/append_crc32.py ${BIG_KERNEL_TEST_BIN}
    COMMENT "Creating big kernel test binary with trailing CRC32"
)

# ============================================================
# F-INFRA I-5: KALLSYMS symbol table (per-target, POST_BUILD regen)
# ============================================================
# Each kernel executable embeds its own {addr,name} table so panic backtraces
# resolve to function names without host addr2line. linker.ld places .rodata
# (where this table lands) AFTER .text, so adding the table never moves any
# recorded function address: a single nm pass over the linked ELF is correct,
# and the POST_BUILD regeneration converges. The generator writes ONLY when
# content changes (preserves mtime), so once stable it triggers no rebuild.
set(KALLSYMS_GEN        ${CMAKE_SOURCE_DIR}/scripts/generate_kallsyms.py)
set(KALLSYMS_DATA_TEST  ${CMAKE_BINARY_DIR}/kernel/kallsyms_data_test.cpp)
set(KALLSYMS_DATA_KERNEL ${CMAKE_BINARY_DIR}/kernel/kallsyms_data_kernel.cpp)

# Seed an empty stub at configure time so the first build compiles. A clean
# checkout therefore needs a second build for the real table to be compiled in
# (CI builds twice). Guarded: do NOT overwrite a previously-regenerated real
# table on reconfigure.
set(_KALLSYMS_STUB [==[
// AUTO-GENERATED placeholder (first build); regenerated POST_BUILD from nm.
#include <stddef.h>
#include "kernel/lib/kallsyms.hpp"
extern "C" const cinux::lib::KallsymEntry g_kallsyms_table[] = {};
extern "C" const size_t g_kallsyms_count = 0;
]==])
if(NOT EXISTS ${KALLSYMS_DATA_TEST})
    file(WRITE ${KALLSYMS_DATA_TEST} "${_KALLSYMS_STUB}")
endif()
if(NOT EXISTS ${KALLSYMS_DATA_KERNEL})
    file(WRITE ${KALLSYMS_DATA_KERNEL} "${_KALLSYMS_STUB}")
endif()

target_sources(big_kernel      PRIVATE ${KALLSYMS_DATA_KERNEL})
target_sources(big_kernel_test PRIVATE ${KALLSYMS_DATA_TEST})
set_source_files_properties(${KALLSYMS_DATA_KERNEL} ${KALLSYMS_DATA_TEST}
                            PROPERTIES GENERATED TRUE)

# Regenerate each table from its own linked ELF after every build.
add_custom_command(TARGET big_kernel POST_BUILD
    COMMAND python3 ${KALLSYMS_GEN} $<TARGET_FILE:big_kernel> ${KALLSYMS_DATA_KERNEL}
    COMMENT "KALLSYMS: regenerate big_kernel symbol table")
add_custom_command(TARGET big_kernel_test POST_BUILD
    COMMAND python3 ${KALLSYMS_GEN} $<TARGET_FILE:big_kernel_test> ${KALLSYMS_DATA_TEST}
    COMMENT "KALLSYMS: regenerate big_kernel_test symbol table")
