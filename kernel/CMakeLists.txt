set(CINUX_ARCHITECTURE "x86_64" CACHE STRING "Architecture directory under kernel/arch")

file(GLOB_RECURSE CINUX_KERNEL_TREE_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/kernel/*.cpp
    ${CMAKE_SOURCE_DIR}/kernel/*.S)

set(CINUX_KERNEL_PORTABLE_SOURCES "")
set(CINUX_KERNEL_ARCH_SOURCES "")
foreach(source IN LISTS CINUX_KERNEL_TREE_SOURCES)
    if(source MATCHES "/arch/")
        if(source MATCHES "/arch/${CINUX_ARCHITECTURE}/")
            list(APPEND CINUX_KERNEL_ARCH_SOURCES ${source})
        endif()
    else()
        list(APPEND CINUX_KERNEL_PORTABLE_SOURCES ${source})
    endif()
endforeach()
list(SORT CINUX_KERNEL_PORTABLE_SOURCES)
list(SORT CINUX_KERNEL_ARCH_SOURCES)

list(REMOVE_ITEM CINUX_KERNEL_PORTABLE_SOURCES ${CMAKE_SOURCE_DIR}/kernel/kernel.cpp)

set(CINUX_KERNEL_CORE_SOURCES
    ${CINUX_KERNEL_PORTABLE_SOURCES}
    ${CINUX_KERNEL_ARCH_SOURCES}
    ${CMAKE_SOURCE_DIR}/base/src/format/entry.cpp
    ${CMAKE_SOURCE_DIR}/base/src/format/radix.cpp
    ${CMAKE_SOURCE_DIR}/boot/early/libc.cpp)

set(CINUX_KERNEL_NO_SSE_ISLAND
    ${CMAKE_SOURCE_DIR}/kernel/arch/x86_64/isr.cpp
    ${CMAKE_SOURCE_DIR}/kernel/arch/x86_64/exception.cpp
    ${CMAKE_SOURCE_DIR}/kernel/arch/x86_64/pic.cpp
    ${CMAKE_SOURCE_DIR}/kernel/arch/x86_64/irq_stubs.cpp
    ${CMAKE_SOURCE_DIR}/kernel/interrupt/irq.cpp
    ${CMAKE_SOURCE_DIR}/kernel/debug/trace.cpp
    ${CMAKE_SOURCE_DIR}/kernel/arch/x86_64/irq_guard.cpp
    ${CMAKE_SOURCE_DIR}/kernel/proc/task.cpp
    ${CMAKE_SOURCE_DIR}/kernel/proc/task_runtime.cpp
    ${CMAKE_SOURCE_DIR}/kernel/proc/scheduler.cpp
    ${CMAKE_SOURCE_DIR}/kernel/proc/sync.cpp)

set_source_files_properties(${CINUX_KERNEL_NO_SSE_ISLAND} PROPERTIES COMPILE_OPTIONS "-mgeneral-regs-only")

add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/font_blob.o
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/assets/font.psf
            ${CMAKE_CURRENT_BINARY_DIR}/font.psf
    COMMAND ${CMAKE_OBJCOPY} -I binary -O elf64-x86-64 -B i386:x86-64
            --rename-section .data=.rodata.font
            --redefine-sym _binary_font_psf_start=g_font_psf_start
            --redefine-sym _binary_font_psf_end=g_font_psf_end
            --set-section-flags .rodata.font=alloc,load,readonly,data
            font.psf
            font_blob.o
    DEPENDS ${CMAKE_SOURCE_DIR}/assets/font.psf
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/user_blob.o
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_BINARY_DIR}/user/shell.bin
            ${CMAKE_CURRENT_BINARY_DIR}/shell.bin
    COMMAND ${CMAKE_OBJCOPY} -I binary -O elf64-x86-64 -B i386:x86-64
            --rename-section .data=usertext.user
            --redefine-sym _binary_shell_bin_start=g_user_shell_start
            --redefine-sym _binary_shell_bin_end=g_user_shell_end
            --set-section-flags usertext.user=alloc,load,readonly,data
            shell.bin
            user_blob.o
    DEPENDS user_shell ${CMAKE_BINARY_DIR}/user/shell.bin
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

function(add_cinux_kernel name bin_stem)
    add_executable(${name}
        ${CMAKE_SOURCE_DIR}/kernel/kernel.cpp
        ${CINUX_KERNEL_CORE_SOURCES}
        ${CMAKE_CURRENT_BINARY_DIR}/font_blob.o
        ${CMAKE_CURRENT_BINARY_DIR}/user_blob.o
        ${ARGN})
    target_link_libraries(${name} PRIVATE cinux_warnings cinux_kernel_flags)
    target_compile_options(${name} PRIVATE -mcmodel=kernel)
    target_include_directories(${name} PRIVATE
        ${CMAKE_SOURCE_DIR}/base/include
        ${CMAKE_SOURCE_DIR})
    target_compile_definitions(${name} PRIVATE
        ENTRY_SYMBOL=KernelEntry)
    target_link_options(${name} PRIVATE
        -m64
        -no-pie
        -nostdlib
        -Wl,-T,${CMAKE_SOURCE_DIR}/kernel/arch/x86_64/kernel.ld
        -Wl,--build-id=none
        -Wl,--no-warn-rwx-segments
        -Wl,--gc-sections
        -Wl,-e,KernelEntry)
    set_property(TARGET ${name} APPEND PROPERTY LINK_DEPENDS
        ${CMAKE_SOURCE_DIR}/kernel/arch/x86_64/kernel.ld)

    add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/kernel/${bin_stem}.bin
        COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${name}>
                ${CMAKE_BINARY_DIR}/kernel/${bin_stem}.bin
        COMMAND ${CMAKE_COMMAND}
                -DIMAGE_FILE=${CMAKE_BINARY_DIR}/kernel/${bin_stem}.bin
                -P ${CMAKE_SOURCE_DIR}/cmake/kernel/image_size_check.cmake
        DEPENDS ${name} ${CMAKE_SOURCE_DIR}/cmake/kernel/image_size_check.cmake
    )

    add_custom_target(${bin_stem}_bin
        DEPENDS ${CMAKE_BINARY_DIR}/kernel/${bin_stem}.bin
    )
endfunction()

function(add_cinux_ktest name stage)
    add_executable(cinux_ktest_${name}
        ${CMAKE_SOURCE_DIR}/test/kernel/test_main.cpp
        ${CINUX_KERNEL_CORE_SOURCES}
        ${CMAKE_CURRENT_BINARY_DIR}/font_blob.o
        ${CMAKE_SOURCE_DIR}/test/framework/framework_kernel.cpp
        ${ARGN})
    target_link_libraries(cinux_ktest_${name} PRIVATE cinux_warnings cinux_kernel_flags)
    target_compile_options(cinux_ktest_${name} PRIVATE -mcmodel=kernel)
    target_include_directories(cinux_ktest_${name} PRIVATE
        ${CMAKE_SOURCE_DIR}/base/include
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/test/framework)
    string(TOUPPER ${stage} stage_upper)
    target_compile_definitions(cinux_ktest_${name} PRIVATE
        ENTRY_SYMBOL=KernelEntry KTEST_STAGE_${stage_upper})
    target_link_options(cinux_ktest_${name} PRIVATE
        -m64
        -no-pie
        -nostdlib
        -Wl,-T,${CMAKE_SOURCE_DIR}/kernel/arch/x86_64/kernel.ld
        -Wl,--build-id=none
        -Wl,--no-warn-rwx-segments
        -Wl,--gc-sections
        -Wl,-e,KernelEntry)
    set_property(TARGET cinux_ktest_${name} APPEND PROPERTY LINK_DEPENDS
        ${CMAKE_SOURCE_DIR}/kernel/arch/x86_64/kernel.ld)

    add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/kernel/cinux_ktest_${name}.bin
        COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:cinux_ktest_${name}>
                ${CMAKE_BINARY_DIR}/kernel/cinux_ktest_${name}.bin
        COMMAND ${CMAKE_COMMAND}
                -DIMAGE_FILE=${CMAKE_BINARY_DIR}/kernel/cinux_ktest_${name}.bin
                -P ${CMAKE_SOURCE_DIR}/cmake/kernel/image_size_check.cmake
        DEPENDS cinux_ktest_${name} ${CMAKE_SOURCE_DIR}/cmake/kernel/image_size_check.cmake
    )

    add_custom_target(cinux_ktest_${name}_bin
        DEPENDS ${CMAKE_BINARY_DIR}/kernel/cinux_ktest_${name}.bin
    )
endfunction()

add_cinux_kernel(cinux_kernel kernel)

configure_file(${CMAKE_SOURCE_DIR}/cmake/debug/kernel.gdb.in
               ${CMAKE_BINARY_DIR}/kernel.gdb @ONLY)

foreach(entry IN LISTS CINUX_KTESTS CINUX_KTESTS_INTEGRATION)
    string(REPLACE ":" ";" entry_parts "${entry}")
    list(GET entry_parts 0 ktest)
    list(GET entry_parts 1 ktest_stage)
    add_cinux_ktest(${ktest} ${ktest_stage} ${CMAKE_SOURCE_DIR}/test/kernel/test_${ktest}.cpp)
endforeach()
