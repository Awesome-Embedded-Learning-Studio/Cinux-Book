# We Need CMake Acquired above 3.16 :)
cmake_minimum_required(VERSION 3.16)

project(Cinux VERSION 0.1.0 LANGUAGES CXX ASM)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 14)
        message(FATAL_ERROR
            "Cinux requires GCC >= 14 (C++23 freestanding + strict warnings); "
            "found GCC ${CMAKE_CXX_COMPILER_VERSION}.")
    endif()
endif()

# We Forced Acquired C++23, Light Weight And fun enough!
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Extensions off :)
set(CMAKE_CXX_EXTENSIONS OFF)

# compile commands could help us start our journey, happily
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(cmake/compilers/warning_standard.cmake)
include(cmake/compilers/boot_modes.cmake)
include(cmake/compilers/kernel_modes.cmake)
include(cmake/checks/options.cmake)

set(CINUX_QEMU_PROGRAM qemu-system-x86_64 CACHE STRING "QEMU binary driving Cinux")
set(CINUX_EXT2_IMG ${CMAKE_BINARY_DIR}/boot/ext2.img)
add_custom_command(
    OUTPUT ${CINUX_EXT2_IMG}
    COMMAND ${CMAKE_COMMAND} -E touch ${CINUX_EXT2_IMG}
    COMMAND ${CMAKE_COMMAND} -E env "CINUX_HELLO_ELF=$<TARGET_FILE:user_hello>"
            "CINUX_SHELL_ELF=$<TARGET_FILE:user_shell>"
            "CINUX_MAPPER_ELF=$<TARGET_FILE:user_mapper>"
            "CINUX_SIGDEMO_ELF=$<TARGET_FILE:user_sigdemo>"
            "CINUX_SIGCATCH_ELF=$<TARGET_FILE:user_sigcatch>"
            "CINUX_NXDEMO_ELF=$<TARGET_FILE:user_nxdemo>"
            "CINUX_EFAULTER_ELF=$<TARGET_FILE:user_efaulter>"
            "CINUX_SMEPDEMO_ELF=$<TARGET_FILE:user_smepdemo>"
            "CINUX_ASLRDEMO_ELF=$<TARGET_FILE:user_aslrdemo>"
            "CINUX_TLSDEMO_ELF=$<TARGET_FILE:user_tlsdemo>"
            "CINUX_THREADDEMO_ELF=$<TARGET_FILE:user_threaddemo>"
            "CINUX_TTYDEMO_ELF=$<TARGET_FILE:user_ttydemo>"
            "CINUX_SMPSTRESS_ELF=$<TARGET_FILE:user_smpstress>"
            bash ${CMAKE_SOURCE_DIR}/scripts/create_ext2_disk.sh ${CINUX_EXT2_IMG}
    DEPENDS user_hello user_shell user_mapper user_sigdemo user_sigcatch user_nxdemo
            user_efaulter user_smepdemo user_aslrdemo
            user_tlsdemo user_threaddemo user_ttydemo user_smpstress
            ${CMAKE_SOURCE_DIR}/scripts/create_ext2_disk.sh
            ${CMAKE_SOURCE_DIR}/scripts/ext2_seed/smp-stress.sh
    COMMENT "teaching ext2 volume for the block and fs experiments")
add_custom_target(ext2_img DEPENDS ${CINUX_EXT2_IMG})
set(CINUX_EXT2_KTEST_IMG ${CMAKE_BINARY_DIR}/boot/ext2_ktest.img)
add_custom_target(ext2_ktest_img
    COMMAND ${CMAKE_COMMAND} -E rm -f ${CINUX_EXT2_KTEST_IMG}
    COMMAND ${CMAKE_COMMAND} -E copy ${CINUX_EXT2_IMG} ${CINUX_EXT2_KTEST_IMG}
    DEPENDS ext2_img
    COMMENT "scratch copy of the ext2 volume for kernel tests")
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
    -device;usb-tablet,bus=xhci0.0)
string(REPLACE ";" "\n" CINUX_QEMU_DEVICE_LINES "${CINUX_QEMU_DEVICE_ARGS}")
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/qemu-device-args.txt"
    CONTENT "${CINUX_QEMU_DEVICE_LINES}\n")

# Kernel test kernels: one entry "name:stage" = one test file
# test/kernel/test_<name>.cpp (integration: test/kernel/integration/) =
# one kernel = one image = one QEMU boot = one CTest case. The stage
# names the init-ladder level the test main walks before running cases.
set(CINUX_KTEST_STAGES exceptions irq keyboard mm proc block)
set(CINUX_KTESTS selfcheck:exceptions tick:irq keyboard:keyboard vmm:mm heap:mm aspace:mm cow:mm
    vma:mm demand:mm loader:mm uaccess:proc sched:proc block:block block_error:block ext2:block
    pipe:proc tty:keyboard smp:proc smp_pipe:proc xhci:block)
set(CINUX_KTESTS_INTEGRATION "")

message("Configuring Boot")
add_subdirectory(boot)

add_subdirectory(base)

add_subdirectory(user)

add_subdirectory(kernel)

add_subdirectory(test)
