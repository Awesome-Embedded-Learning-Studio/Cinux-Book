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
        framework/assert_host.cpp
        framework/irq_guard_host.cpp)
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
add_cinux_test(idt)
add_cinux_test(layout)
add_cinux_test(mm_layout)
add_cinux_test(vmm)
add_cinux_test(page)
add_cinux_test(image)
add_cinux_test(bitmap)
add_cinux_test(font)
add_cinux_test(framebuffer)
add_cinux_test(console)
add_cinux_test(keyboard ${CMAKE_SOURCE_DIR}/kernel/driver/scancode.cpp)
add_cinux_test(pmm ${CMAKE_SOURCE_DIR}/kernel/mm/pmm.cpp)
add_cinux_test(heap ${CMAKE_SOURCE_DIR}/kernel/mm/heap.cpp)
add_cinux_test(tick)
add_cinux_test(context ${CMAKE_SOURCE_DIR}/kernel/proc/task.cpp)
add_cinux_test(self_list)
add_cinux_test(sync ${CMAKE_SOURCE_DIR}/kernel/proc/task.cpp
                      ${CMAKE_SOURCE_DIR}/kernel/proc/scheduler.cpp
                      ${CMAKE_SOURCE_DIR}/kernel/proc/sync.cpp)
add_cinux_test(guard)
add_cinux_test(scheduler ${CMAKE_SOURCE_DIR}/kernel/proc/task.cpp
                          ${CMAKE_SOURCE_DIR}/kernel/proc/scheduler.cpp
                          ${CMAKE_SOURCE_DIR}/kernel/proc/sync.cpp)

add_test(NAME kernel_image
    COMMAND ${CMAKE_COMMAND}
            -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
            -DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/kernel_image
            -DCXX_COMPILER=${CMAKE_CXX_COMPILER}
            -DOBJCOPY=${CMAKE_OBJCOPY}
            -P ${CMAKE_SOURCE_DIR}/cmake/test/test_kernel_image.cmake)

# Ktest registry entries are "name:stage": the stage names the highest
# ladder level (kernel init order) the test kernel brings up before its
# cases run. Module tests live in test/kernel/, integration tests in
# test/kernel/integration/ — each entry one file, one kernel, one boot.
set(CINUX_KTEST_IMAGE_TARGETS "")
foreach(entry IN LISTS CINUX_KTESTS CINUX_KTESTS_INTEGRATION)
    string(REPLACE ":" ";" entry_parts "${entry}")
    list(GET entry_parts 0 ktest)
    list(GET entry_parts 1 ktest_stage)
    list(FIND CINUX_KTEST_STAGES "${ktest_stage}" stage_known)
    if(stage_known EQUAL -1)
        message(FATAL_ERROR
            "ktest '${ktest}' declares unknown stage '${ktest_stage}'; known: ${CINUX_KTEST_STAGES}")
    endif()
    add_test(NAME kernel_test_${ktest}
        COMMAND ${CMAKE_COMMAND}
                -DQEMU_PROGRAM=${CINUX_QEMU_PROGRAM}
                "-DQEMU_DEVICE_ARGS=${CINUX_QEMU_DEVICE_ARGS}"
                -DIMAGE_FILE=${CMAKE_BINARY_DIR}/boot/cinux_ktest_${ktest}.img
                -DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/kernel_test_${ktest}
                -P ${CMAKE_SOURCE_DIR}/cmake/test/run_kernel_tests.cmake)
    set_tests_properties(kernel_test_${ktest} PROPERTIES TIMEOUT 120)
    list(APPEND CINUX_KTEST_IMAGE_TARGETS boot_ktest_${ktest})
endforeach()

add_custom_target(test_host
    COMMAND ${CMAKE_CTEST_COMMAND} --test-dir ${CMAKE_CURRENT_BINARY_DIR}
            -E ^kernel_test_ --output-on-failure
    DEPENDS ${ALL_HOST_TESTS}
    USES_TERMINAL
    COMMENT "Running host unit tests..."
)

add_custom_target(test_kernel
    COMMAND ${CMAKE_CTEST_COMMAND} --test-dir ${CMAKE_CURRENT_BINARY_DIR}
            -R ^kernel_test_ --output-on-failure
    DEPENDS ${CINUX_KTEST_IMAGE_TARGETS}
    USES_TERMINAL
    COMMENT "Running kernel tests in QEMU..."
)
