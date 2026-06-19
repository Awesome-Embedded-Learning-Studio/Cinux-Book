target_sources(big_kernel_common PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/kprintf.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/klog.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/string.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/kallsyms.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ubsan.cpp
)
