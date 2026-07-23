target_sources(big_kernel_common PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/process_new.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/task_builder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/fork.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/clone.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/execve.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/elf_load.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/elf_types.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/scheduler.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/scheduler_block.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/roundrobin.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sync.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/lockdep.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/percpu.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/init.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/user_launch.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/pid.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/signal.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/signal_frame.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/process_group.cpp
)

# §14: shell_launch.cpp is the non-GUI userspace entry (fork + exec /bin/sh).
# GUI builds link kernel/gui/desktop_launch.cpp instead (the gui/ subdirectory is
# added only under if(CINUX_GUI) in kernel/CMakeLists.txt).  Exactly one of the
# two launch_userspace() impls is linked.
if(NOT CINUX_GUI)
    target_sources(big_kernel_common PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/shell_launch.cpp
    )
endif()
