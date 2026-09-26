enable_testing()

set(TEST_INCLUDE_DIRS
    ${CMAKE_SOURCE_DIR}/test/framework
    ${CMAKE_SOURCE_DIR}/base/include
)

function(add_cinux_test name)
    add_executable(test_${name}
        unit/test_${name}.cpp
        framework/framework.cpp
        framework/assert_host.cpp)
    target_include_directories(test_${name} PRIVATE ${TEST_INCLUDE_DIRS})
    target_link_libraries(test_${name} PRIVATE cinux_base cinux_warnings)
    add_test(NAME ${name} COMMAND test_${name})
    list(APPEND ALL_HOST_TESTS test_${name})
    set(ALL_HOST_TESTS ${ALL_HOST_TESTS} PARENT_SCOPE)
endfunction()

add_cinux_test(format)
add_cinux_test(framework)
add_cinux_test(result)

add_custom_target(test_host
    COMMAND ${CMAKE_CTEST_COMMAND} --test-dir ${CMAKE_CURRENT_BINARY_DIR} --output-on-failure
    DEPENDS ${ALL_HOST_TESTS}
    USES_TERMINAL
    COMMENT "Running host unit tests..."
)
