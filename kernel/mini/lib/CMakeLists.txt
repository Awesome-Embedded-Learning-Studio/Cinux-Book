# ==============================================================
# Cinux Mini Kernel - Library Build Configuration
# ==============================================================

# 私有实现库（格式化函数）
add_library(kprintf_private STATIC
    private/format.cpp
)

target_compile_options(kprintf_private PRIVATE
    -ffreestanding
    -fno-exceptions
    -fno-rtti
)

target_include_directories(kprintf_private PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
