cmake_minimum_required(VERSION 3.20)

project(cinux 
    VERSION 0.1.0 
    LANGUAGES C CXX ASM)

option(CINUX_GUI "Enable GUI mode" ON)

# F-INFRA I-10: opt-in lockdep -- assert no spinlock is held across schedule()
# (would deadlock single-core). Off by default (zero cost); enable for deadlock
# hunting: cmake -DCINUX_LOCKDEP=ON ...
option(CINUX_LOCKDEP "Enable lockdep schedule-while-locked debug checks" OFF)

# F-INFRA I-9: opt-in UBSan. -fsanitize=undefined instruments the kernel to call
# freestanding __ubsan_handle_* stubs (kernel/lib/ubsan.cpp) that kpanic on UB.
# Compile-only (no libubsan at link -- the stubs resolve the references). Off by
# default; enable for UB hunting: cmake -DCINUX_UBSAN=ON ...
option(CINUX_UBSAN "Enable -fsanitize=undefined with freestanding UBSan stubs" OFF)

# Toolchain version gate (G2). CinuxOS is freestanding C++17 with -mcmodel=kernel
# codegen, inline asm, and (F-INFRA) GCC-sensitive UBSAN (-fsanitize=undefined) and
# strict warning flags whose behavior varies across major GCC releases. Fail fast at
# configure time if GCC predates the tested floor so a too-old toolchain does not
# silently miscompile or mis-instrument. CI runs on a current GCC; this mainly
# protects local/older setups. (Clang is not the configured toolchain; left ungated.)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 11)
        message(FATAL_ERROR
            "CinuxOS requires GCC >= 11 (C++17 freestanding + strict warnings + UBSAN); "
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
add_subdirectory(kernel)

include(cmake/qemu.cmake)

if(CINUX_BUILD_TESTS)
    add_subdirectory(test)
endif()

message(STATUS "")
message(STATUS "=== Cinux Build Configuration ===")
message(STATUS "  Project:     ${PROJECT_NAME} v${PROJECT_VERSION}")
message(STATUS "  Build type:  ${CMAKE_BUILD_TYPE}")
message(STATUS "  C Compiler:  ${CMAKE_C_COMPILER}")
message(STATUS "  CXX Compiler:${CMAKE_CXX_COMPILER}")
message(STATUS "  ASM Compiler:${CMAKE_ASM_COMPILER}")
message(STATUS "  Toolchain:   ${CMAKE_TOOLCHAIN_FILE}")
message(STATUS "  Build tests: ${CINUX_BUILD_TESTS}")
message(STATUS "  GUI mode:    ${CINUX_GUI}")
message(STATUS "===================================")
message(STATUS "")
