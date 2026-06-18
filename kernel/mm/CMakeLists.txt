target_sources(big_kernel_common PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/pmm.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/buddy.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vmm.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/heap.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/address_space.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vma.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/page_cache.cpp
)
