# ==============================================================
# Cinux Mini Kernel - Minimal Build
# ==============================================================

# ==============================================================
# Mini Kernel Common Compile Options
# ==============================================================
# 确保生产内核和测试内核使用完全一致的编译选项
set(MINI_KERNEL_COMMON_COMPILE_OPTIONS
    -ffreestanding
    -fno-exceptions
    -fno-rtti
    -fno-pie
    -mcmodel=large
    -mno-red-zone
    -Wall
)

set(MINI_KERNEL_COMMON_LINK_OPTIONS
    -T ${CMAKE_CURRENT_SOURCE_DIR}/linker.ld
    -nostdlib
    -no-pie
)

# ==============================================================
# Production Kernel
# ==============================================================

add_executable(mini_kernel
    arch/x86_64/boot.S
    arch/x86_64/crt_stub.cpp
    main.cpp
    driver/serial.cpp
    lib/kprintf.cpp
    lib/private/format.cpp
)

# Set output directory to build/kernel/mini/
set_target_properties(mini_kernel PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/kernel/mini
    OUTPUT_NAME "mini_kernel"
)

target_compile_options(mini_kernel PRIVATE ${MINI_KERNEL_COMMON_COMPILE_OPTIONS})

# Add include directories for kernel headers
target_include_directories(mini_kernel PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_options(mini_kernel PRIVATE ${MINI_KERNEL_COMMON_LINK_OPTIONS})

# Post-build: Convert ELF to flat binary
add_custom_command(TARGET mini_kernel
    POST_BUILD
    COMMAND objcopy -O binary $<TARGET_FILE:mini_kernel> $<TARGET_FILE_DIR:mini_kernel>/mini_kernel.bin
    COMMENT "Converting mini kernel to flat binary: mini_kernel.bin"
    VERBATIM
)

# ==============================================================
# Subdirectories
# ==============================================================

# Library build
add_subdirectory(lib)

# Tests (only when CINUX_BUILD_TESTS is ON)
if(CINUX_BUILD_TESTS)
    add_subdirectory(test)
endif()
