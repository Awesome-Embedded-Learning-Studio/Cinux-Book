# Cinux-Base — CMake root
# Zero-OS-coupling C++17 base library for CinuxOS

cmake_minimum_required(VERSION 3.16)
project(cinux_base VERSION 0.1.0 LANGUAGES CXX)

# ============ C++ Standard ============

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# ============ Header-only Interface Library ============

add_library(cinux_base_headers INTERFACE)
target_include_directories(cinux_base_headers INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(cinux_base_headers INTERFACE cxx_std_17)
target_compile_options(cinux_base_headers INTERFACE
    -Wall -Wextra -Wpedantic -Werror
    -Wold-style-cast -Wshadow
    -Wnon-virtual-dtor -Woverloaded-virtual
    -fno-exceptions -fno-rtti
)

# ============ Static Library (.hpp + .cpp components) ============

file(GLOB_RECURSE CINUX_BASE_SOURCES src/*.cpp)

if(CINUX_BASE_SOURCES)
    add_library(cinux_base STATIC ${CINUX_BASE_SOURCES})
    target_link_libraries(cinux_base PUBLIC cinux_base_headers)
    target_compile_options(cinux_base PRIVATE
        -Wall -Wextra -Wpedantic -Werror
        -Wold-style-cast -Wshadow
        -Wnon-virtual-dtor -Woverloaded-virtual
        -fno-exceptions -fno-rtti
    )
else()
    # No .cpp sources yet — provide an INTERFACE stub so test targets can link unconditionally
    add_library(cinux_base ALIAS cinux_base_headers)
endif()

# ============ Tests ============

option(CINUX_BUILD_TESTS "Build Cinux-Base tests" ON)

if(CINUX_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()
