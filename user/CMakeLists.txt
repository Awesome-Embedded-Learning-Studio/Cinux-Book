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
    -nostdlib
    -static
    -no-pie
    -Wl,-e,_start
    -lgcc)
set_target_properties(user_shell PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/user)

set_target_properties(user_shell PROPERTIES OUTPUT_NAME "shell.elf")

add_executable(user_hello programs/hello.cpp)
target_compile_options(user_hello PRIVATE ${USER_COMPILE_FLAGS})
target_include_directories(user_hello PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR})
target_compile_definitions(user_hello PRIVATE __USER__)
target_link_options(user_hello PRIVATE
    -nostdlib
    -static
    -no-pie
    -Wl,-e,_start
    -lgcc)
set_target_properties(user_hello PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/user
    OUTPUT_NAME "hello.elf")

add_executable(user_mapper programs/mapper.cpp)
target_compile_options(user_mapper PRIVATE ${USER_COMPILE_FLAGS})
target_include_directories(user_mapper PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR})
target_compile_definitions(user_mapper PRIVATE __USER__)
target_link_options(user_mapper PRIVATE
    -nostdlib
    -static
    -no-pie
    -Wl,-e,_start
    -lgcc)
set_target_properties(user_mapper PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/user
    OUTPUT_NAME "mapper.elf")
