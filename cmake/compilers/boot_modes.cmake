foreach(mode 16 32 64)
    add_library(cinux_boot${mode} INTERFACE)
    target_compile_options(cinux_boot${mode} INTERFACE
        -m${mode}
        -Os
        -ffreestanding
        -fno-pie
        -fno-pic
        -fno-stack-protector
        -fno-asynchronous-unwind-tables
        -fno-ident
        -fno-exceptions
        -fno-rtti
        -fno-threadsafe-statics
        -mgeneral-regs-only
    )
endforeach()

target_compile_options(cinux_boot64 INTERFACE
    -mno-red-zone
)
