cmake_minimum_required(VERSION 3.16)
project(MiniPrimes VERSION 1.0 LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_executable(mini-primes main.cpp primes.cpp)
target_compile_features(mini-primes PRIVATE cxx_std_17)
set_target_properties(mini-primes PROPERTIES CXX_EXTENSIONS OFF)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(mini-primes PRIVATE -Wall -Wextra -Wpedantic)
endif()
