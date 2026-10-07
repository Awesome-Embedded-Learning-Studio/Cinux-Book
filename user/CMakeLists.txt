set(USER_COMPILE_FLAGS
    -ffreestanding
    -fno-exceptions
    -fno-rtti
    -fno-pie
    -fno-stack-protector
    -mcmodel=small
    -mno-red-zone
    -std=c++23
    -Wall
    -Wextra
    -O2)

add_executable(user_shell programs/shell.cpp)
target_compile_options(user_shell PRIVATE ${USER_COMPILE_FLAGS})
target_include_directories(user_shell PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR})
target_compile_definitions(user_shell PRIVATE __USER__)
target_link_options(user_shell PRIVATE
    -T ${CMAKE_CURRENT_SOURCE_DIR}/linker.ld
    -nostdlib
    -static
    -no-pie
    -lgcc)
set_target_properties(user_shell PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/user
    OUTPUT_NAME "shell")

add_custom_command(TARGET user_shell POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:user_shell> ${CMAKE_BINARY_DIR}/user/shell.bin
    COMMENT "Converting user shell to flat binary"
    VERBATIM)
