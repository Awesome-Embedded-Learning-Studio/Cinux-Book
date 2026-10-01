add_executable(cinux_kernel
    kernel.cpp
    arch/x86_64/entry.cpp
    arch/x86_64/exception.cpp
    arch/x86_64/image_header_data.cpp
    ${CMAKE_SOURCE_DIR}/base/src/format/entry.cpp
    ${CMAKE_SOURCE_DIR}/base/src/format/radix.cpp
    ${CMAKE_SOURCE_DIR}/boot/early/libc.cpp)
target_link_libraries(cinux_kernel PRIVATE cinux_warnings cinux_boot64)
target_compile_options(cinux_kernel PRIVATE -mcmodel=kernel)
target_include_directories(cinux_kernel PRIVATE
    ${CMAKE_SOURCE_DIR}/base/include
    ${CMAKE_SOURCE_DIR})
target_compile_definitions(cinux_kernel PRIVATE
    ENTRY_SYMBOL=KernelEntry)
target_link_options(cinux_kernel PRIVATE
    -m64
    -no-pie
    -nostdlib
    -Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/arch/x86_64/kernel.ld
    -Wl,--build-id=none
    -Wl,--no-warn-rwx-segments
    -Wl,--gc-sections
    -Wl,-e,KernelEntry)

add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/kernel.bin
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:cinux_kernel>
            ${CMAKE_CURRENT_BINARY_DIR}/kernel.bin
    DEPENDS cinux_kernel
)

add_custom_target(kernel_bin
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/kernel.bin
)
