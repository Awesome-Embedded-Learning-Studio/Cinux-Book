# Cinux-Base — Test configuration
# Uses Catch2 v3 via FetchContent

include(FetchContent)

# ============ Catch2 v3 ============

FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.7.1
)
FetchContent_MakeAvailable(Catch2)

# Catch2 provides catch_discover_tests via this module
list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(CTest)
include(Catch)

# ============ Test Helper ============

# add_cinux_test(name)
#   Creates test_<name> executable from test/test_<name>.cpp
#   Links header-only headers. Auto-registers with CTest.
function(add_cinux_test name)
    add_executable(test_${name} ${CMAKE_CURRENT_SOURCE_DIR}/test_${name}.cpp)
    target_link_libraries(test_${name} PRIVATE
        cinux_base_headers
        Catch2::Catch2WithMain
    )
    target_compile_options(test_${name} PRIVATE
        -Wall -Wextra -Wpedantic -Werror
        -Wshadow -Wnon-virtual-dtor
        -Wno-sign-conversion -Wno-conversion
        -fno-exceptions -fno-rtti
    )
    catch_discover_tests(test_${name})
endfunction()

# add_cinux_cpp_test(name)
#   Same but also links cinux_base static library
#   (for tests that depend on .cpp components: crc32, checksum, logger, etc.)
function(add_cinux_cpp_test name)
    add_executable(test_${name} ${CMAKE_CURRENT_SOURCE_DIR}/test_${name}.cpp)
    target_link_libraries(test_${name} PRIVATE
        cinux_base
        Catch2::Catch2WithMain
    )
    target_compile_options(test_${name} PRIVATE
        -Wall -Wextra -Wpedantic -Werror
        -Wshadow -Wnon-virtual-dtor
        -Wno-sign-conversion -Wno-conversion
        -fno-exceptions -fno-rtti
    )
    catch_discover_tests(test_${name})
endfunction()

# ============ Sprint 0: Smoke ============

add_cinux_test(smoke)

# ============ Sprint 1: Leaf Components ============

add_cinux_test(scope_guard)
add_cinux_test(numeric)
add_cinux_test(bit_ops)
add_cinux_test(endian)
add_cinux_test(expected)
add_cinux_cpp_test(crc32)

# ============ Sprint 2: Basic Types ============

add_cinux_test(string_view)
add_cinux_test(span)
add_cinux_test(optional)
add_cinux_test(static_string)
add_cinux_test(bitmap)

# ============ Sprint 3: Containers & Tools ============

add_cinux_test(intrusive_list)
add_cinux_test(function)
add_cinux_test(ring_buffer)
add_cinux_test(buffer)
add_cinux_test(static_hash_map)
add_cinux_cpp_test(checksum)

# ============ Sprint 4: High-level + Migration ============

add_cinux_cpp_test(vformat)
add_cinux_test(algorithm)
add_cinux_cpp_test(logger)
add_cinux_cpp_test(random)

# ============ Sprint 5: Integration ============

# add_cinux_test(integration)

# ============ Custom Targets ============

add_custom_target(test_all
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    USES_TERMINAL
    COMMENT "Running all Cinux-Base tests..."
)

add_custom_target(test_verbose
    COMMAND ${CMAKE_CTEST_COMMAND} --verbose --output-on-failure
    COMMENT "Running tests in verbose mode..."
)
