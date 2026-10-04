# We Need CMake Acquired above 3.16 :)
cmake_minimum_required(VERSION 3.16)

project(Cinux VERSION 0.1.0 LANGUAGES CXX)

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

set(CINUX_QEMU_PROGRAM qemu-system-x86_64 CACHE STRING "QEMU binary driving Cinux")
set(CINUX_QEMU_DEVICE_ARGS
    -accel;kvm
    -display;none
    -no-reboot
    -device;isa-debug-exit,iobase=0xf4,iosize=0x04)

# Kernel test kernels: one entry "name:stage" = one test file
# test/kernel/test_<name>.cpp (integration: test/kernel/integration/) =
# one kernel = one image = one QEMU boot = one CTest case. The stage
# names the init-ladder level the test main walks before running cases.
set(CINUX_KTEST_STAGES exceptions irq)
set(CINUX_KTESTS selfcheck:exceptions tick:irq)
set(CINUX_KTESTS_INTEGRATION "")

message("Configuring Boot")
add_subdirectory(boot)

add_subdirectory(base)

add_subdirectory(kernel)

add_subdirectory(test)
