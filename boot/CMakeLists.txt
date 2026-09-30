add_executable(mbr mbr.cpp)
target_link_libraries(mbr PRIVATE cinux_warnings cinux_boot16)
target_include_directories(mbr PRIVATE ${CMAKE_SOURCE_DIR}/base/include)
target_link_options(mbr PRIVATE
    -m32
    -no-pie
    -nostdlib
    -Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/mbr.ld
    -Wl,--build-id=none
)
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/mbr.bin
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:mbr>
            ${CMAKE_CURRENT_BINARY_DIR}/mbr.bin
    DEPENDS mbr
)

add_custom_target(mbr_bin
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/mbr.bin
)

file(READ ${CMAKE_CURRENT_SOURCE_DIR}/layout.hpp layout_src)
string(REGEX MATCH "\\.sectors = ([0-9]+)" layout_match "${layout_src}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/layout.hpp)

add_library(boot_pm OBJECT
    pm/pm.cpp)
target_link_libraries(boot_pm PRIVATE cinux_warnings cinux_boot32)
target_include_directories(boot_pm PRIVATE
    ${CMAKE_SOURCE_DIR}/base/include
    ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(stage2
    stage2.cpp
    bios.cpp
    gdt/gdt.cpp
    pm/pm_switch.cpp
    early/libc.cpp
    ${CMAKE_SOURCE_DIR}/base/src/format/entry.cpp
    ${CMAKE_SOURCE_DIR}/base/src/format/radix.cpp
    $<TARGET_OBJECTS:boot_pm>)
target_link_libraries(stage2 PRIVATE cinux_warnings cinux_boot16)
target_include_directories(stage2 PRIVATE
    ${CMAKE_SOURCE_DIR}/base/include
    ${CMAKE_SOURCE_DIR}/base/src)
target_compile_options(stage2 PRIVATE -ffunction-sections)
target_link_options(stage2 PRIVATE
    -m32
    -no-pie
    -nostdlib
    -Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/stage2.ld
    -Wl,--build-id=none
    -Wl,--gc-sections
)

add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/stage2.bin
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:stage2>
            ${CMAKE_CURRENT_BINARY_DIR}/stage2.bin
    DEPENDS stage2
)

add_custom_target(stage2_bin
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/stage2.bin
)

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
