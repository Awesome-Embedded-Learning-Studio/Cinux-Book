file(SIZE "${IMAGE_FILE}" image_bytes)
if(image_bytes LESS 40)
    message(FATAL_ERROR "Kernel image is shorter than its header: ${image_bytes} bytes")
endif()

file(READ "${IMAGE_FILE}" file_size_hex OFFSET 16 LIMIT 8 HEX)
string(REGEX MATCHALL ".." file_size_bytes "${file_size_hex}")
list(REVERSE file_size_bytes)
string(JOIN "" file_size_hex ${file_size_bytes})
math(EXPR declared_bytes "0x${file_size_hex}")

if(NOT declared_bytes EQUAL image_bytes)
    message(FATAL_ERROR
        "Kernel file_size (${declared_bytes}) differs from binary size (${image_bytes}): ${IMAGE_FILE}")
endif()
message(STATUS "Kernel image size verified: ${image_bytes} bytes")
