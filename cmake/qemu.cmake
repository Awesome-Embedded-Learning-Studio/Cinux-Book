find_program(QEMU_EXECUTABLE qemu-system-x86_64)

if(NOT QEMU_EXECUTABLE)
    set(QEMU_EXECUTABLE "qemu-system-x86_64")
    message(WARNING "qemu-system-x86_64 not found in PATH, using default name")
endif()

set(QEMU_COMMON_FLAGS
    -m 512M
    -serial stdio
    -no-reboot
    -debugcon file:debug.log
    -global isa-debugcon.iobase=0xe9
)

set(QEMU_DEVELOP_FLAG     
    -no-shutdown)

# QEMU 额外测试标志（添加到 COMMON_FLAGS 之上）
set(QEMU_TEST_EXTRA_FLAGS
    -device isa-debug-exit,iobase=0xf4,iosize=0x04
)

# 将 CMake list 转换为空格分隔的字符串（用于脚本生成）
string(REPLACE ";" " " QEMU_COMMON_FLAGS_STR "${QEMU_COMMON_FLAGS}")
string(REPLACE ";" " " QEMU_TEST_EXTRA_FLAGS_STR "${QEMU_TEST_EXTRA_FLAGS}")
set(QEMU_COMMON_FLAGS_STR "${QEMU_COMMON_FLAGS_STR}" CACHE INTERNAL "")
set(QEMU_TEST_EXTRA_FLAGS_STR "${QEMU_TEST_EXTRA_FLAGS_STR}" CACHE INTERNAL "")

# Set the debug console as 0xe9
# -s: GDB stub on :1234
# -S: Stop at startup (for debugging)
set(QEMU_DEBUG_FLAGS
    -s
    -S
)

if(NOT CINUX_IMAGE_PATH)
    message(STATUS "Image Path not specified yet, using default")
    set(CINUX_IMAGE_PATH "${CMAKE_BINARY_DIR}/cinux.img" CACHE PATH "Cinux disk image path")
endif()

# Let We make boots before sessions
set(MBR_BIN    "${CMAKE_BINARY_DIR}/boot/mbr.bin")
set(STAGE2_BIN "${CMAKE_BINARY_DIR}/boot/stage2.bin")
set(MINI_BIN   "${CMAKE_BINARY_DIR}/kernel/mini/mini_kernel.bin")
add_custom_command(
    OUTPUT ${CINUX_IMAGE_PATH}
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/build_image.sh
        ${MBR_BIN}
        ${STAGE2_BIN}
        ${MINI_BIN}
        ${CINUX_IMAGE_PATH}
    DEPENDS mbr stage2 mini_kernel
    COMMENT "Building disk image: ${CINUX_IMAGE_PATH}"
    VERBATIM
)

add_custom_target(image ALL
    DEPENDS ${CINUX_IMAGE_PATH}
)

add_custom_target(run
    COMMAND ${QEMU_EXECUTABLE} ${QEMU_COMMON_FLAGS} ${QEMU_DEVELOP_FLAG} 
        -drive file=${CINUX_IMAGE_PATH},format=raw,index=0,media=disk
    DEPENDS image
    COMMENT "Starting QEMU (serial: stdio)"
    VERBATIM
)

add_custom_target(run-debug
    COMMAND ${QEMU_EXECUTABLE} ${QEMU_COMMON_FLAGS} ${QEMU_DEVELOP_FLAG} ${QEMU_DEBUG_FLAGS}
        -drive file=${CINUX_IMAGE_PATH},format=raw,index=0,media=disk
    DEPENDS image
    COMMENT "Starting QEMU in debug mode (GDB on :1234)"
    VERBATIM
)


add_custom_target(run-gdb
    COMMAND gdb -ex "target remote :1234" build/kernel.elf
    DEPENDS run-debug
    COMMENT "Using gdb to connects for debugings"
    VERBATIM)

# ==============================================================
# Test Kernel Targets
# ==============================================================

set(MINI_TEST_BIN "${CMAKE_BINARY_DIR}/kernel/mini/mini_kernel_test.bin")

# 测试内核磁盘镜像
set(CINUX_TEST_IMAGE_PATH "${CMAKE_BINARY_DIR}/cinux_test.img")

add_custom_command(
    OUTPUT ${CINUX_TEST_IMAGE_PATH}
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/build_image.sh
        ${MBR_BIN}
        ${STAGE2_BIN}
        ${MINI_TEST_BIN}
        ${CINUX_TEST_IMAGE_PATH}
    DEPENDS mbr stage2 mini_kernel_test
    COMMENT "Building test disk image: ${CINUX_TEST_IMAGE_PATH}"
    VERBATIM
)

add_custom_target(test-image
    DEPENDS ${CINUX_TEST_IMAGE_PATH}
)

# 运行测试内核（自动退出模式）
add_custom_target(run-kernel-test
    COMMAND ${QEMU_EXECUTABLE} ${QEMU_COMMON_FLAGS} ${QEMU_TEST_EXTRA_FLAGS}
        -drive file=${CINUX_TEST_IMAGE_PATH},format=raw,index=0,media=disk
    DEPENDS test-image
    USES_TERMINAL
    COMMENT "Starting QEMU with TEST kernel (auto-exit)"
    VERBATIM
)

# 测试内核调试模式
add_custom_target(run-kernel-test-debug
    COMMAND ${QEMU_EXECUTABLE} ${QEMU_COMMON_FLAGS} ${QEMU_DEBUG_FLAGS}
        -drive file=${CINUX_TEST_IMAGE_PATH},format=raw,index=0,media=disk
    DEPENDS test-image
    COMMENT "Starting QEMU with TEST kernel in debug mode (GDB on :1234)"
    VERBATIM
)

# 交互式运行测试内核（需要 Ctrl+C 退出）
add_custom_target(run-kernel-test-interactive
    COMMAND ${QEMU_EXECUTABLE} ${QEMU_COMMON_FLAGS}
        -drive file=${CINUX_TEST_IMAGE_PATH},format=raw,index=0,media=disk
    DEPENDS test-image
    USES_TERMINAL
    COMMENT "Starting QEMU with TEST kernel (interactive, Ctrl+C to exit)"
    VERBATIM
)

message(STATUS "QEMU targets:")
message(STATUS "  make run        : Start QEMU normally")
message(STATUS "  make run-debug  : Start QEMU with GDB server on :1234")
message(STATUS "  make image      : Build disk image only")
message(STATUS "  make run-gdb    : Connects the qemu automatically")
message(STATUS "")
message(STATUS "Test Kernel targets:")
message(STATUS "  make run-kernel-test            : Run test kernel (auto-exit)")
message(STATUS "  make run-kernel-test-interactive : Run test kernel (needs Ctrl+C)")
message(STATUS "  make run-kernel-test-debug      : Run test kernel with GDB")
message(STATUS "  make test-image                  : Build test disk image only")
message(STATUS "")
message(STATUS "Unified Testing:")
message(STATUS "  make test                  : Run ALL tests (host + kernel, auto-exit)")
message(STATUS "  make test_host             : Run host unit tests only")
message(STATUS "  make test_verbose          : Run host tests in verbose mode")