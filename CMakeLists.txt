# We Need CMake Acquired above 3.16 :)
cmake_minimum_required(VERSION 3.16)

list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/platform")
project(Cinux VERSION 0.1.0 LANGUAGES C CXX ASM)

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
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# compile commands could help us start our journey, happily
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(cmake/compilers/warning_standard.cmake)
include(cmake/compilers/boot_modes.cmake)
include(cmake/compilers/kernel_modes.cmake)
include(cmake/checks/options.cmake)

find_program(CINUX_PYTHON_EXECUTABLE python3)
if(NOT CINUX_PYTHON_EXECUTABLE)
    message(FATAL_ERROR "Cinux build tools require Python 3")
endif()
set(CINUX_COMPILE_JOBS 2 CACHE STRING "Ninja compile pool size")
set(CINUX_LINK_JOBS 1 CACHE STRING "Ninja link pool size")
set_property(GLOBAL PROPERTY JOB_POOLS cinux_compile=${CINUX_COMPILE_JOBS} cinux_link=${CINUX_LINK_JOBS})
set(CMAKE_JOB_POOL_COMPILE cinux_compile)
set(CMAKE_JOB_POOL_LINK cinux_link)

include(cmake/test/kernel_catalog.cmake)
add_subdirectory(boot)
add_subdirectory(base)
add_subdirectory(user)
add_subdirectory(kernel)
include(cmake/rootfs.cmake)
add_subdirectory(user/rootfs/buildroot)
include(cmake/qemu/devices.cmake)
add_subdirectory(test EXCLUDE_FROM_ALL)
include(cmake/qemu/run.cmake)
