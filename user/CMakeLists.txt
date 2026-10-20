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

set(USER_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/base/include)

add_executable(user_shell programs/shell.cpp)
target_compile_options(user_shell PRIVATE ${USER_COMPILE_FLAGS})
target_include_directories(user_shell PRIVATE ${USER_INCLUDE_DIRS})
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
target_include_directories(user_hello PRIVATE ${USER_INCLUDE_DIRS})
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
target_include_directories(user_mapper PRIVATE ${USER_INCLUDE_DIRS})
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

add_executable(user_sigdemo programs/sigdemo.cpp)
target_compile_options(user_sigdemo PRIVATE ${USER_COMPILE_FLAGS})
target_include_directories(user_sigdemo PRIVATE ${USER_INCLUDE_DIRS})
target_compile_definitions(user_sigdemo PRIVATE __USER__)
target_link_options(user_sigdemo PRIVATE
    -nostdlib
    -static
    -no-pie
    -Wl,-e,_start
    -lgcc)
set_target_properties(user_sigdemo PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/user
    OUTPUT_NAME "sigdemo.elf")

add_executable(user_tlsdemo programs/tlsdemo.cpp)
target_compile_options(user_tlsdemo PRIVATE ${USER_COMPILE_FLAGS})
target_include_directories(user_tlsdemo PRIVATE ${USER_INCLUDE_DIRS})
target_compile_definitions(user_tlsdemo PRIVATE __USER__)
target_link_options(user_tlsdemo PRIVATE
    -nostdlib
    -static
    -no-pie
    -Wl,-e,_start
    -lgcc)
set_target_properties(user_tlsdemo PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/user
    OUTPUT_NAME "tlsdemo.elf")

add_executable(user_threaddemo programs/threaddemo.cpp)
target_compile_options(user_threaddemo PRIVATE ${USER_COMPILE_FLAGS})
target_include_directories(user_threaddemo PRIVATE ${USER_INCLUDE_DIRS})
target_compile_definitions(user_threaddemo PRIVATE __USER__)
target_link_options(user_threaddemo PRIVATE
    -nostdlib
    -static
    -no-pie
    -Wl,-e,_start
    -lgcc)
set_target_properties(user_threaddemo PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/user
    OUTPUT_NAME "threaddemo.elf")

add_executable(user_ttydemo programs/ttydemo.cpp)
target_compile_options(user_ttydemo PRIVATE ${USER_COMPILE_FLAGS})
target_include_directories(user_ttydemo PRIVATE ${USER_INCLUDE_DIRS})
target_compile_definitions(user_ttydemo PRIVATE __USER__)
target_link_options(user_ttydemo PRIVATE
    -nostdlib
    -static
    -no-pie
    -Wl,-e,_start
    -lgcc)
set_target_properties(user_ttydemo PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/user
    OUTPUT_NAME "ttydemo.elf")
