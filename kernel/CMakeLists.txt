add_executable(cinux_kernel
    kernel.cpp
    arch/x86_64/entry.cpp
    arch/x86_64/exception.cpp
    arch/x86_64/gdt.cpp
    arch/x86_64/idt.cpp
    arch/x86_64/isr.cpp
    arch/x86_64/image_header_data.cpp
    ${CMAKE_SOURCE_DIR}/base/src/format/entry.cpp
    ${CMAKE_SOURCE_DIR}/base/src/format/radix.cpp
    ${CMAKE_SOURCE_DIR}/boot/early/libc.cpp
    driver/base/io.cpp
    driver/serial.cpp
    mm/pmm.cpp)
target_link_libraries(cinux_kernel PRIVATE cinux_warnings cinux_kernel_flags)
target_compile_options(cinux_kernel PRIVATE -mcmodel=kernel)
set_source_files_properties(arch/x86_64/isr.cpp PROPERTIES COMPILE_OPTIONS "-mgeneral-regs-only")
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
set_property(TARGET cinux_kernel APPEND PROPERTY LINK_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/arch/x86_64/kernel.ld)

add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/kernel.bin
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:cinux_kernel>
            ${CMAKE_CURRENT_BINARY_DIR}/kernel.bin
    COMMAND ${CMAKE_COMMAND}
            -DIMAGE_FILE=${CMAKE_CURRENT_BINARY_DIR}/kernel.bin
            -P ${CMAKE_SOURCE_DIR}/cmake/kernel/image_size_check.cmake
    DEPENDS cinux_kernel ${CMAKE_SOURCE_DIR}/cmake/kernel/image_size_check.cmake
)

add_custom_target(kernel_bin
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/kernel.bin
)
