string(REPLACE "${CINUX_EXT2_IMG}" "${CINUX_QEMU_ROOTFS}"
       CINUX_QEMU_RUN_ARGS "${CINUX_QEMU_DEVICE_ARGS}")
list(APPEND CINUX_QEMU_RUN_ARGS -m ${CINUX_QEMU_MEMORY}M)
string(REPLACE ";" "\n" CINUX_QEMU_RUN_LINES "${CINUX_QEMU_RUN_ARGS}")
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/qemu-run-args.txt"
    CONTENT "${CINUX_QEMU_RUN_LINES}\n")

add_custom_target(run
    COMMAND ${CMAKE_COMMAND} -DROOTFS=${CINUX_QEMU_ROOTFS}
            -P ${CMAKE_SOURCE_DIR}/cmake/qemu/check_rootfs.cmake
    COMMAND ${CINUX_QEMU_PROGRAM} ${CINUX_QEMU_RUN_ARGS}
            -drive format=raw,file=${CMAKE_BINARY_DIR}/boot/cinux.img
            -serial stdio
    DEPENDS boot_image qemu_disks
    USES_TERMINAL
    VERBATIM
)

add_custom_target(run_debug
    COMMAND ${CMAKE_COMMAND} -DROOTFS=${CINUX_QEMU_ROOTFS}
            -P ${CMAKE_SOURCE_DIR}/cmake/qemu/check_rootfs.cmake
    COMMAND ${CMAKE_COMMAND} -E echo
            "gdb stub on :1234, machine frozen at reset; attach with: gdb -q -x ${CMAKE_BINARY_DIR}/kernel.gdb"
    COMMAND ${CINUX_QEMU_PROGRAM} ${CINUX_QEMU_RUN_ARGS}
            -drive format=raw,file=${CMAKE_BINARY_DIR}/boot/cinux.img
            -serial stdio -S -s
    DEPENDS boot_image qemu_disks
    USES_TERMINAL
    VERBATIM
)

# The GUI lane pairs its flags with values on purpose: REMOVE_ITEM by
# value would strip the first "-device" word it meets, orphaning that
# device's option string. Keep the tablet; the keyboard remains PS/2.
set(CINUX_QEMU_GUI_ARGS "")
set(_gui_source ${CINUX_QEMU_RUN_ARGS})
list(LENGTH _gui_source _gui_total)
set(_gui_index 0)
while(_gui_index LESS _gui_total)
    list(GET _gui_source ${_gui_index} _gui_flag)
    if(_gui_flag STREQUAL "-device" OR _gui_flag STREQUAL "-display")
        math(EXPR _gui_next "${_gui_index} + 1")
        list(GET _gui_source ${_gui_next} _gui_value)
        if(_gui_flag STREQUAL "-device" AND
           NOT _gui_value MATCHES "usb-kbd")
            list(APPEND CINUX_QEMU_GUI_ARGS -device ${_gui_value})
        endif()
        math(EXPR _gui_index "${_gui_index} + 2")
    else()
        list(APPEND CINUX_QEMU_GUI_ARGS ${_gui_flag})
        math(EXPR _gui_index "${_gui_index} + 1")
    endif()
endwhile()
list(APPEND CINUX_QEMU_GUI_ARGS -display gtk,gl=off,zoom-to-fit=on
    -fw_cfg name=opt/cinux/init-env,string=CINUX_SESSION=gui)
string(REPLACE ";" "\n" CINUX_QEMU_GUI_LINES "${CINUX_QEMU_GUI_ARGS}")
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/qemu-gui-args.txt"
    CONTENT "${CINUX_QEMU_GUI_LINES}\n")

add_custom_target(run_gui
    COMMAND ${CMAKE_COMMAND} -DROOTFS=${CINUX_QEMU_ROOTFS}
            -P ${CMAKE_SOURCE_DIR}/cmake/qemu/check_rootfs.cmake
    COMMAND ${CINUX_QEMU_PROGRAM} ${CINUX_QEMU_GUI_ARGS}
            -drive format=raw,file=${CMAKE_BINARY_DIR}/boot/cinux.img
            -serial stdio
    DEPENDS boot_image qemu_disks
    USES_TERMINAL
    VERBATIM
)
add_dependencies(ext2_img qemu_disks)
add_dependencies(test_kernel qemu_disks)
if(CINUX_QEMU_ROOTFS STREQUAL CINUX_BUILDROOT_VOLUME)
    add_dependencies(run buildroot_volume)
    add_dependencies(run_debug buildroot_volume)
    add_dependencies(run_gui buildroot_volume)
endif()
