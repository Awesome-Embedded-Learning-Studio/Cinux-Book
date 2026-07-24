target_sources(big_kernel_common PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/pmm.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/buddy.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vmm.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/address_space.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vma.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/page_cache.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/slab.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dedicated_caches.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/diagnostics.cpp
)

# B1 gcc-stutter profiling: §14 file gate. ON links the real periodic stats
# kthread (stats_kthread.cpp); OFF links the empty stub (stats_kthread_stub.cpp)
# so init.cpp's start_stats_thread() call resolves either way -- no #ifdef in
# source. The PF counter + dump_memory_stats PF line are unconditional (built
# into page_fault.cpp / diagnostics.cpp); only the periodic thread is gated.
if(CINUX_STATS_KTHREAD)
    target_sources(big_kernel_common PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/stats_kthread.cpp)
else()
    target_sources(big_kernel_common PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/stats_kthread_stub.cpp)
endif()
