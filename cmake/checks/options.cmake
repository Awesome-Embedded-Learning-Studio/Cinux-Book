option(CINUX_HOST_ASAN "Host tests: AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(CINUX_HOST_TSAN "Host tests: ThreadSanitizer (separate from ASan)" OFF)
option(CINUX_LOCKDEP "Check spinlock acquisition order in host and kernel" OFF)
option(CINUX_UBSAN "Kernel: UndefinedBehaviorSanitizer traps" OFF)

if(CINUX_HOST_ASAN AND CINUX_HOST_TSAN)
    message(FATAL_ERROR "ASan/UBSan and TSan require separate build directories")
endif()

add_library(cinux_host_checks INTERFACE)
if(CINUX_HOST_ASAN)
    target_compile_options(cinux_host_checks INTERFACE
        -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -g)
    target_link_options(cinux_host_checks INTERFACE -fsanitize=address,undefined)
elseif(CINUX_HOST_TSAN)
    target_compile_options(cinux_host_checks INTERFACE
        -fsanitize=thread -fno-omit-frame-pointer -g)
    target_link_options(cinux_host_checks INTERFACE -fsanitize=thread)
endif()

set(CINUX_LOCKDEP_BOOL false)
if(CINUX_LOCKDEP)
    set(CINUX_LOCKDEP_BOOL true)
endif()
configure_file(${CMAKE_SOURCE_DIR}/cmake/checks/checks_config.hpp.in
    ${CMAKE_BINARY_DIR}/generated/kernel/proc/checks_config.hpp @ONLY)
include_directories(${CMAKE_BINARY_DIR}/generated)

if(CINUX_UBSAN)
    target_compile_options(cinux_kernel_flags INTERFACE
        -fsanitize=undefined -fsanitize-undefined-trap-on-error)
endif()
