# Just the CMake Warning Requests!

add_library(cinux_warnings INTERFACE)

# Export the Cinux Warning Flags
target_compile_options(cinux_warnings INTERFACE
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Werror
)
