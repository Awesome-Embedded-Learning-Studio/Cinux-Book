include(${CMAKE_SOURCE_DIR}/cmake/boot/boot_target.cmake)

add_executable(mbr mbr.cpp)
add_cinux_boot_binary(mbr 16 mbr.ld)

file(READ ${CMAKE_CURRENT_SOURCE_DIR}/layout.hpp layout_src)
string(REGEX MATCH "\\.sectors = ([0-9]+)" layout_match "${layout_src}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/layout.hpp)

add_library(boot_pm OBJECT
    pm/pm.cpp
    lm/lm_switch.cpp
    lm/page_tables.cpp)
target_link_libraries(boot_pm PRIVATE cinux_warnings cinux_boot32)
target_include_directories(boot_pm PRIVATE
    ${CMAKE_SOURCE_DIR}/base/include
    ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(lm_blob lm/lm.cpp)
add_cinux_boot_binary(lm_blob 64 lm/lm.ld)

add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/lm_blob.tmp.o
           ${CMAKE_CURRENT_BINARY_DIR}/lm_blob.o
    COMMAND ${CMAKE_OBJCOPY} -I binary -O elf32-i386 -B i386
            --rename-section .data=.lm_blob
            ${CMAKE_CURRENT_BINARY_DIR}/lm.bin
            ${CMAKE_CURRENT_BINARY_DIR}/lm_blob.tmp.o
    COMMAND ${CMAKE_OBJCOPY} --set-section-flags .lm_blob=alloc,load,readonly,data
            ${CMAKE_CURRENT_BINARY_DIR}/lm_blob.tmp.o
            ${CMAKE_CURRENT_BINARY_DIR}/lm_blob.o
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/lm.bin)
add_custom_target(lm_blob_obj
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/lm_blob.o)

add_executable(stage2
    stage2.cpp
    bios.cpp
    gdt/gdt.cpp
    pm/pm_switch.cpp
    early/libc.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/lm_blob.o
    ${CMAKE_SOURCE_DIR}/base/src/format/entry.cpp
    ${CMAKE_SOURCE_DIR}/base/src/format/radix.cpp
    $<TARGET_OBJECTS:boot_pm>)
add_cinux_boot_binary(stage2 16 stage2.ld -Wl,--gc-sections)
target_include_directories(stage2 PRIVATE ${CMAKE_SOURCE_DIR}/base/src)
target_compile_options(stage2 PRIVATE -ffunction-sections)
add_dependencies(stage2 lm_blob_obj)

add_custom_target(stage2_size_check
    COMMAND ${CMAKE_COMMAND}
            -DSIZE_FILE=${CMAKE_CURRENT_BINARY_DIR}/stage2.bin
            -DMAX_SECTORS=${CMAKE_MATCH_1}
            -P ${CMAKE_SOURCE_DIR}/cmake/boot/stage2_size_check.cmake
    DEPENDS stage2_bin
)

add_custom_target(boot_image
    COMMAND ${CMAKE_COMMAND} -E remove ${CMAKE_CURRENT_BINARY_DIR}/cinux.img
    COMMAND dd if=/dev/zero of=${CMAKE_CURRENT_BINARY_DIR}/cinux.img bs=1M count=1 status=none
    COMMAND dd if=${CMAKE_CURRENT_BINARY_DIR}/mbr.bin
            of=${CMAKE_CURRENT_BINARY_DIR}/cinux.img bs=512 count=1 conv=notrunc status=none
    COMMAND dd if=${CMAKE_CURRENT_BINARY_DIR}/stage2.bin
            of=${CMAKE_CURRENT_BINARY_DIR}/cinux.img bs=512 seek=1 conv=notrunc status=none
    DEPENDS mbr_bin stage2_size_check
)

add_custom_target(run
    COMMAND qemu-system-x86_64
            -drive format=raw,file=${CMAKE_CURRENT_BINARY_DIR}/cinux.img
            -display none
            -debugcon stdio
            -global isa-debugcon.iobase=0xe9
            -no-reboot
    DEPENDS boot_image
)
