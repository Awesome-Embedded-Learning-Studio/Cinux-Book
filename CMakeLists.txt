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

add_subdirectory(base)
add_subdirectory(test)
