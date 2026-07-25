cmake_minimum_required(VERSION 3.20)

project(cinux
    VERSION 1.0.0
    LANGUAGES C CXX ASM)

# All user-facing build switches live in the single registry below. Add a new
# option there (plus a CINUX_COMPILE_DEF_OPTS entry if it maps 1:1 to a compile
# definition) -- not here. Included before any if() that reads a CINUX_ flag.
include(cmake/options.cmake)

# Toolchain version gate (G2). Cinux is freestanding C++17 with -mcmodel=kernel
# codegen, inline asm, and (F-INFRA) GCC-sensitive UBSAN (-fsanitize=undefined) and
# strict warning flags whose behavior varies across major GCC releases. Fail fast at
# configure time if GCC predates the tested floor so a too-old toolchain does not
# silently miscompile or mis-instrument. CI runs on a current GCC; this mainly
# protects local/older setups. (Clang is not the configured toolchain; left ungated.)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 11)
        message(FATAL_ERROR
            "Cinux requires GCC >= 11 (C++17 freestanding + strict warnings + UBSAN); "
            "found GCC ${CMAKE_CXX_COMPILER_VERSION}.")
    endif()
endif()

if(NOT CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE
        "${CMAKE_SOURCE_DIR}/cmake/toolchain-x86_64.cmake"
        CACHE FILEPATH "Toolchain file")
    message(STATUS "Using default toolchain file: ${CMAKE_TOOLCHAIN_FILE}")
endif()

add_compile_options(
    -Wall
    -Wextra
    # Keep frame pointers in ALL build types (including -O2 Release) so the
    # RBP chain stays intact and kernel/user backtraces remain walkable in
    # the production kernel. Aligns with Linux CONFIG_FRAME_POINTER=y. The
    # small code-gen cost is intentional: debuggability beats micro-perf for
    # a hobby OS (FO observability milestone, batch 0).
    -fno-omit-frame-pointer
)

# Export CompileCommands
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_C_FLAGS_DEBUG "-g -O0")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0")
# Release enables _FORTIFY_SOURCE=2 explicitly to match CI's Ubuntu gcc spec, so
# local non-Ubuntu toolchains reproduce CI's fortified <cstring>/<string.h>
# (catches __memcpy_chk misuse locally, not only in CI; see PLAN GOTCHA #9).
# Requires the -O2 above; Ubuntu spec would add it implicitly anyway, so this is
# a no-op on CI but a hardening parity fix everywhere else.
set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG -D_FORTIFY_SOURCE=2")
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG -D_FORTIFY_SOURCE=2")

enable_testing()

add_subdirectory(boot)
add_subdirectory(user)
# F13 cinux-gui: host-neutral core is now a git submodule (third_party/Cinux-GUI),
# added BEFORE kernel/ so the cinux-gui target exists when the kernel links it.
if(CINUX_GUI)
    add_subdirectory(third_party/Cinux-GUI)
    # F13/PF-gate: cinux-gui is added BEFORE kernel/, so it does NOT inherit
    # kernel/CMakeLists.txt's -mstack-protector-guard=global. Without this the
    # core's canary reads fs:0x28 (TLS default), which faults in the kernel
    # (no TLS canary slot); the legacy PF handler's zero-page demand-paging
    # masked it (read 0, canary 0==0 false-OK), so pump() silently ran with a
    # dead canary. Point the canary at the kernel's global __stack_chk_guard.
    target_compile_options(cinux-gui PRIVATE
        -mstack-protector-guard=global
        # F13-B: match the kernel's freestanding C++ discipline. cinux-gui is
        # added at the root, so it does NOT inherit kernel/CMakeLists.txt's
        # -fno-rtti -fno-exceptions. Without this the widget tree's virtual
        # dispatch generates typeinfo referencing __cxxabiv1 symbols the kernel
        # does not link (crt_stub provides __cxa_atexit/guard/pure_virtual +
        # operator new/delete, but no class_type_info vtable).
        -fno-rtti
        -fno-exceptions)
endif()
add_subdirectory(kernel)

include(cmake/qemu.cmake)

if(CINUX_BUILD_TESTS)
    add_subdirectory(test)
endif()

message(STATUS "")
message(STATUS "=== Cinux Build Configuration ===")
message(STATUS "  Project:      ${PROJECT_NAME} v${PROJECT_VERSION}")
message(STATUS "  Build type:   ${CMAKE_BUILD_TYPE}")
message(STATUS "  C Compiler:   ${CMAKE_C_COMPILER}")
message(STATUS "  CXX Compiler: ${CMAKE_CXX_COMPILER}")
message(STATUS "  ASM Compiler: ${CMAKE_ASM_COMPILER}")
message(STATUS "  Toolchain:    ${CMAKE_TOOLCHAIN_FILE}")
message(STATUS "  -- Feature switches (add one in cmake/options.cmake) --")
foreach(_o IN ITEMS GUI USB NET BUILD_TESTS
                    LOCKDEP UBSAN
                    MUSL_HELLO_SMOKE MUSL_DYN_SMOKE BUSYBOX_SMOKE GCC_TOOLCHAIN
                    HOST_ASAN HOST_TSAN)
    message(STATUS "  CINUX_${_o} = ${CINUX_${_o}}")
endforeach()
message(STATUS "  -- Paths / runtime --")
message(STATUS "  IMAGE_PATH:        ${CINUX_IMAGE_PATH}")
message(STATUS "  ROOTFS:            ${CINUX_ROOTFS_BUILDROOT_IMG}")
message(STATUS "  CINUX_NO_KVM: $ENV{CINUX_NO_KVM}   CI: $ENV{CI}")
message(STATUS "===================================")
message(STATUS "")
