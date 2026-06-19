# third_party/CMakeLists.txt
#
# @brief Integrate Cinux-Base as a submodule dependency.
#
# Exposes only the include path and .cpp sources — does NOT call
# add_subdirectory(Cinux-Base) to avoid its INTERFACE compile flags
# (-Wpedantic -Werror -Wold-style-cast -Wshadow) leaking into
# big_kernel_common and breaking existing kernel code.

set(CINUX_BASE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/Cinux-Base)

# Allow kernel code to write: #include <cinux/expected.hpp> etc.
target_include_directories(big_kernel_common PUBLIC
    ${CINUX_BASE_DIR}/include
)

# Compile Cinux-Base .cpp sources (crc32, checksum, logger, vformat)
# as part of the kernel object library.
file(GLOB_RECURSE CINUX_BASE_SOURCES
    ${CINUX_BASE_DIR}/src/*.cpp
)
target_sources(big_kernel_common PRIVATE ${CINUX_BASE_SOURCES})

# F-INFRA I-9: never instrument Cinux-Base with UBSan. It is -Wpedantic -Werror
# and heap-free; -fsanitize=undefined may surface intentional sub-source UB and
# the stubs here must not apply to another repo's contract code.
if(CINUX_UBSAN)
    set_source_files_properties(${CINUX_BASE_SOURCES} PROPERTIES COMPILE_OPTIONS "-fno-sanitize=undefined")
endif()
