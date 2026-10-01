function(add_cinux_boot_binary target mode script)
    if(mode STREQUAL "64")
        set(link_mode 64)
    else()
        set(link_mode 32)
    endif()

    target_link_libraries(${target} PRIVATE cinux_warnings cinux_boot${mode})
    target_include_directories(${target} PRIVATE
        ${CMAKE_SOURCE_DIR}/base/include
        ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_options(${target} PRIVATE
        -m${link_mode}
        -no-pie
        -nostdlib
        -Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/${script}
        -Wl,--build-id=none
        -Wl,--no-warn-rwx-segments
        ${ARGN})
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${target}.bin
        COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${target}>
                ${CMAKE_CURRENT_BINARY_DIR}/${target}.bin
        DEPENDS ${target})
    add_custom_target(${target}_bin
        DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/${target}.bin)
endfunction()
