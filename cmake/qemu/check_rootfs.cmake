if(NOT EXISTS "${ROOTFS}")
    message(FATAL_ERROR
        "run/run_debug/run_gui boot from '${ROOTFS}', which does not exist.\n"
        "The default development volume is built automatically when missing. "
        "A custom -DCINUX_QEMU_ROOTFS=<path> must point at an existing volume.")
endif()
