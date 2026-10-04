function(run_checked)
    execute_process(COMMAND ${ARGV}
        RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "Command failed: ${ARGV}\n${output}\n${error}")
    endif()
endfunction()

file(MAKE_DIRECTORY "${TEST_DIR}")
run_checked("${CXX_COMPILER}" -m64 -c
    "-I${SOURCE_DIR}"
    "${SOURCE_DIR}/kernel/arch/x86_64/image_header_data.cpp"
    -o "${TEST_DIR}/header.o")

foreach(layout text_only rodata rodata_data)
    set(fixture ".text\n.global KernelEntry\nKernelEntry:\n.byte 0xc3\n")
    if(NOT layout STREQUAL "text_only")
        string(APPEND fixture ".section .rodata,\"a\"\n.balign 64\n.byte 0x42\n")
    endif()
    if(layout STREQUAL "rodata_data")
        string(APPEND fixture ".data\n.balign 128\n.byte 0x99\n")
    endif()
    string(APPEND fixture ".bss\n.balign 32\n.zero 4096\n")
    file(WRITE "${TEST_DIR}/${layout}.S" "${fixture}")
    run_checked("${CXX_COMPILER}" -m64 -c "${TEST_DIR}/${layout}.S"
        -o "${TEST_DIR}/${layout}.o")
    run_checked("${CXX_COMPILER}" -m64 -no-pie -nostdlib
        "-Wl,-T,${SOURCE_DIR}/kernel/arch/x86_64/kernel.ld"
        -Wl,--build-id=none -Wl,--no-warn-rwx-segments
        "${TEST_DIR}/header.o" "${TEST_DIR}/${layout}.o"
        -o "${TEST_DIR}/${layout}.elf")
    run_checked("${OBJCOPY}" -O binary
        "${TEST_DIR}/${layout}.elf" "${TEST_DIR}/${layout}.bin")
    run_checked("${CMAKE_COMMAND}" "-DIMAGE_FILE=${TEST_DIR}/${layout}.bin"
        -P "${SOURCE_DIR}/cmake/kernel/image_size_check.cmake")
endforeach()

file(READ "${TEST_DIR}/rodata.bin" truncated LIMIT 48)
file(WRITE "${TEST_DIR}/truncated.bin" "${truncated}")
configure_file("${TEST_DIR}/rodata.bin" "${TEST_DIR}/extended.bin" COPYONLY)
file(APPEND "${TEST_DIR}/extended.bin" "x")
foreach(invalid truncated extended)
    execute_process(COMMAND "${CMAKE_COMMAND}" "-DIMAGE_FILE=${TEST_DIR}/${invalid}.bin"
        -P "${SOURCE_DIR}/cmake/kernel/image_size_check.cmake"
        RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(status EQUAL 0 OR NOT error MATCHES "differs from binary size")
        message(FATAL_ERROR "Size check did not reject ${invalid} image:\n${output}\n${error}")
    endif()
endforeach()
