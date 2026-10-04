# Runs the kernel-world test image under QEMU and asserts the exit code.
# The kernel leaves via isa-debug-exit with code = failures + 1, so QEMU
# exits with 2*(failures + 1) + 1: 3 means every case passed, any other
# value names the failure shape. The serial log is the autopsy report.

file(MAKE_DIRECTORY "${TEST_DIR}")

if(NOT EXISTS "/dev/kvm")
    message(FATAL_ERROR
        "/dev/kvm is missing: Cinux runs QEMU with -accel kvm only, no TCG fallback")
endif()

set(serial_log "${TEST_DIR}/serial.log")
file(REMOVE "${serial_log}")

execute_process(
    COMMAND ${QEMU_PROGRAM} ${QEMU_DEVICE_ARGS}
            -drive format=raw,file=${IMAGE_FILE}
            -serial file:${serial_log}
    RESULT_VARIABLE qemu_status
    TIMEOUT 60
    OUTPUT_QUIET ERROR_VARIABLE qemu_stderr)

if(NOT qemu_status EQUAL 3 AND NOT qemu_stderr STREQUAL "")
    message(WARNING "QEMU diagnostics: ${qemu_stderr}")
endif()

function(dump_serial_and_fail reason)
    if(EXISTS "${serial_log}")
        file(READ "${serial_log}" serial_output)
        message(FATAL_ERROR "kernel tests failed: ${reason}\n--- serial log ---\n${serial_output}")
    endif()
    message(FATAL_ERROR "kernel tests failed: ${reason} (no serial log at ${serial_log})")
endfunction()

if(qemu_status EQUAL 3)
    message(STATUS "kernel tests: all passed (serial log: ${serial_log})")
elseif(qemu_status MATCHES "^[0-9]+$" AND qemu_status GREATER_EQUAL 3)
    math(EXPR failed_cases "(${qemu_status} - 3) / 2")
    dump_serial_and_fail("QEMU exit ${qemu_status}, ${failed_cases} failing case(s)")
elseif(qemu_status EQUAL 1)
    dump_serial_and_fail("QEMU exit 1: the kernel never reached the exit port (early death)")
else()
    dump_serial_and_fail("QEMU ended with '${qemu_status}' (hang or crash)")
endif()
