# ==============================================================
# Cinux Mini Kernel - Test Build
# ==============================================================

add_executable(mini_kernel_test
    ../arch/x86_64/boot.S
    ../arch/x86_64/crt_stub.cpp
    main_test.cpp           # 测试专用 main
    ../driver/serial.cpp
    ../lib/kprintf.cpp
    ../lib/private/format.cpp
    test_cpp_basic.cpp
)

# 使用与生产内核完全相同的编译选项
target_compile_options(mini_kernel_test PRIVATE ${MINI_KERNEL_COMMON_COMPILE_OPTIONS})

target_include_directories(mini_kernel_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

# 使用与生产内核完全相同的链接选项
target_link_options(mini_kernel_test PRIVATE ${MINI_KERNEL_COMMON_LINK_OPTIONS})

set_target_properties(mini_kernel_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/kernel/mini
    OUTPUT_NAME "mini_kernel_test"
)

add_custom_command(TARGET mini_kernel_test
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/kernel/mini
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:mini_kernel_test> ${CMAKE_BINARY_DIR}/kernel/mini/mini_kernel_test.bin
    COMMENT "Converting mini kernel test to flat binary"
    VERBATIM
)
