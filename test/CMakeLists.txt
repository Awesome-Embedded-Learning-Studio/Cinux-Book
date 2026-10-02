enable_testing()

set(TEST_INCLUDE_DIRS
    ${CMAKE_SOURCE_DIR}/test/framework
    ${CMAKE_SOURCE_DIR}/base/include
    ${CMAKE_SOURCE_DIR}
)

function(add_cinux_test name)
    add_executable(test_${name}
        unit/test_${name}.cpp
        ${ARGN}
        framework/framework.cpp
        framework/assert_host.cpp)
    target_include_directories(test_${name} PRIVATE ${TEST_INCLUDE_DIRS})
    target_link_libraries(test_${name} PRIVATE cinux_base cinux_warnings)
    add_test(NAME ${name} COMMAND test_${name})
    list(APPEND ALL_HOST_TESTS test_${name})
    set(ALL_HOST_TESTS ${ALL_HOST_TESTS} PARENT_SCOPE)
endfunction()

add_cinux_test(e820)
add_cinux_test(format)
add_cinux_test(framework)
add_cinux_test(result)
add_cinux_test(vesa)
add_cinux_test(bits)
add_cinux_test(gdt)
add_cinux_test(layout)
add_cinux_test(page)
add_cinux_test(image)
add_cinux_test(bitmap)
add_cinux_test(pmm ${CMAKE_SOURCE_DIR}/kernel/mm/pmm.cpp)

add_test(NAME kernel_image
    COMMAND ${CMAKE_COMMAND}
            -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
            -DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/kernel_image
            -DCXX_COMPILER=${CMAKE_CXX_COMPILER}
            -DOBJCOPY=${CMAKE_OBJCOPY}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_kernel_image.cmake)

add_custom_target(test_host
    COMMAND ${CMAKE_CTEST_COMMAND} --test-dir ${CMAKE_CURRENT_BINARY_DIR} --output-on-failure
    DEPENDS ${ALL_HOST_TESTS}
    USES_TERMINAL
    COMMENT "Running host unit tests..."
)
