set(CINUX_QEMU_PROGRAM qemu-system-x86_64 CACHE STRING "QEMU binary driving Cinux")
set(CINUX_QEMU_ROOTFS "${CINUX_BUILDROOT_VOLUME}"
    CACHE FILEPATH "Root filesystem used by run, run_debug and run_gui")
set(CINUX_QEMU_MEMORY 1024 CACHE STRING "QEMU runtime memory in MiB")
set(CINUX_QEMU_DEVICE_ARGS
    -accel;kvm
    -cpu;host
    -smp;2
    -display;none
    -no-reboot
    -device;isa-debug-exit,iobase=0xf4,iosize=0x04
    -device;ahci,id=sata0
    -drive;format=raw,file=${CINUX_EXT2_IMG},if=none,id=spare
    -device;ide-hd,drive=spare,bus=sata0.0
    -device;qemu-xhci,id=xhci0
    -device;usb-kbd,bus=xhci0.0
    -device;usb-tablet,bus=xhci0.0
    -netdev;user,id=net0
    -device;e1000,netdev=net0)
option(CINUX_QEMU_NVME "Attach a scratch NVMe namespace" ON)
if(CINUX_QEMU_NVME)
    set(CINUX_NVME_IMG ${CMAKE_BINARY_DIR}/boot/nvme-scratch.img)
    add_custom_command(OUTPUT ${CINUX_NVME_IMG}
        COMMAND ${CINUX_PYTHON_EXECUTABLE} ${CMAKE_SOURCE_DIR}/cmake/qemu/create_disk.py
                ${CINUX_NVME_IMG} 8388608
        VERBATIM)
    add_custom_target(qemu_disks DEPENDS ${CINUX_NVME_IMG})
    list(APPEND CINUX_QEMU_DEVICE_ARGS
        -drive;format=raw,file=${CINUX_NVME_IMG},if=none,id=nvme_disk
        -device;nvme,drive=nvme_disk,serial=cinux-nvme)
else()
    add_custom_target(qemu_disks)
endif()
string(REPLACE ";" "\n" CINUX_QEMU_DEVICE_LINES "${CINUX_QEMU_DEVICE_ARGS}")
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/qemu-device-args.txt"
    CONTENT "${CINUX_QEMU_DEVICE_LINES}\n")
