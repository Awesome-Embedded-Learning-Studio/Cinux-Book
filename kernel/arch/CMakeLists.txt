target_sources(big_kernel_common PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/boot.S
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/crt_stub.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/gdt.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/idt.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/interrupts.S
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/exception_handlers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/fault_diag.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/page_fault.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/backtrace.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/irq_backend.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/irq_handlers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/pic.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/paging.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/tlb.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/context_switch.S
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/usermode.S
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/usermode.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/tls.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/syscall.S
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/syscall.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/ap_trampoline.S
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/ap_main.cpp
)

# B3 defect C stage2: TLB drain kthread (deferred CoW shootdown+free).  §14
# file gate: ON links tlb_drain.cpp, OFF links tlb_drain_stub.cpp (empty
# start_tlb_drain_thread); no source #ifdef.
if(CINUX_TLB_DRAIN)
    target_sources(big_kernel_common PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/tlb_drain.cpp)
else()
    target_sources(big_kernel_common PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/tlb_drain_stub.cpp)
endif()
