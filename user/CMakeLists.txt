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

add_executable(user_sigcatch programs/sigcatch.cpp)
target_compile_options(user_sigcatch PRIVATE ${USER_COMPILE_FLAGS})
target_include_directories(user_sigcatch PRIVATE ${USER_INCLUDE_DIRS})
target_compile_definitions(user_sigcatch PRIVATE __USER__)
target_link_options(user_sigcatch PRIVATE
    -nostdlib
    -static
    -no-pie
    -Wl,-e,_start
    -lgcc)
set_target_properties(user_sigcatch PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/user
    OUTPUT_NAME "sigcatch.elf")

add_executable(user_nxdemo programs/nxdemo.cpp)
target_compile_options(user_nxdemo PRIVATE ${USER_COMPILE_FLAGS})
target_include_directories(user_nxdemo PRIVATE ${USER_INCLUDE_DIRS})
target_compile_definitions(user_nxdemo PRIVATE __USER__)
target_link_options(user_nxdemo PRIVATE
    -nostdlib
    -static
    -no-pie
    -Wl,-e,_start
    -lgcc)
set_target_properties(user_nxdemo PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/user
    OUTPUT_NAME "nxdemo.elf")

add_executable(user_efaulter programs/efaulter.cpp)
target_compile_options(user_efaulter PRIVATE ${USER_COMPILE_FLAGS})
target_include_directories(user_efaulter PRIVATE ${USER_INCLUDE_DIRS})
target_compile_definitions(user_efaulter PRIVATE __USER__)
target_link_options(user_efaulter PRIVATE
    -nostdlib
    -static
    -no-pie
    -Wl,-e,_start
    -lgcc)
set_target_properties(user_efaulter PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/user
    OUTPUT_NAME "efaulter.elf")

add_executable(user_smepdemo programs/smepdemo.cpp)
target_compile_options(user_smepdemo PRIVATE ${USER_COMPILE_FLAGS})
target_include_directories(user_smepdemo PRIVATE ${USER_INCLUDE_DIRS})
target_compile_definitions(user_smepdemo PRIVATE __USER__)
target_link_options(user_smepdemo PRIVATE
    -nostdlib
    -static
    -no-pie
    -Wl,-e,_start
    -lgcc)
set_target_properties(user_smepdemo PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/user
    OUTPUT_NAME "smepdemo.elf")

add_executable(user_aslrdemo programs/aslrdemo.cpp)
target_compile_options(user_aslrdemo PRIVATE ${USER_COMPILE_FLAGS})
target_include_directories(user_aslrdemo PRIVATE ${USER_INCLUDE_DIRS})
target_compile_definitions(user_aslrdemo PRIVATE __USER__)
target_link_options(user_aslrdemo PRIVATE
    -nostdlib
    -static
    -no-pie
    -Wl,-e,_start
    -lgcc)
set_target_properties(user_aslrdemo PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/user
    OUTPUT_NAME "aslrdemo.elf")

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

add_executable(user_smpstress programs/smpstress.cpp
    ${CMAKE_SOURCE_DIR}/boot/early/libc.cpp
    ${CMAKE_SOURCE_DIR}/base/src/format/entry.cpp
    ${CMAKE_SOURCE_DIR}/base/src/format/radix.cpp)
target_compile_options(user_smpstress PRIVATE ${USER_COMPILE_FLAGS})
target_include_directories(user_smpstress PRIVATE ${USER_INCLUDE_DIRS})
target_link_options(user_smpstress PRIVATE -nostdlib -static -no-pie -Wl,-e,SmpStressStart -lgcc)
set_target_properties(user_smpstress PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/user
    OUTPUT_NAME "smpstress.elf")
