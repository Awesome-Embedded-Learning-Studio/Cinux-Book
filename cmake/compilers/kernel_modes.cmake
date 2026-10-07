add_library(cinux_kernel_flags INTERFACE)
target_compile_options(cinux_kernel_flags INTERFACE
    -m64
    -O2
    -ffreestanding
    -fno-pie
    -fno-pic
    -fno-stack-protector
    -fno-asynchronous-unwind-tables
    -fno-ident
    -fno-exceptions
    -fno-rtti
    -fno-threadsafe-statics
    -mno-red-zone
    -fno-omit-frame-pointer
    -ftree-vectorize
)
