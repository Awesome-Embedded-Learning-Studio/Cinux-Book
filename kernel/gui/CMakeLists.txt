#
# kernel/gui/CMakeLists.txt
#
# @brief GUI subsystem CMake configuration (F13-B)
#
# F-GUI-USERSPACE (2026-07-08): the in-kernel host adapter (host_cinux.cpp) is
# deleted -- the widget tree + Host ABI table + GuiCore pump now live entirely in
# the USERSPACE GUI host (user/cinux_gui_host, fork+execve'd by desktop_launch).
# kernel/gui/ keeps only the kernel-side plumbing the userspace host can't do:
#   - event.cpp        : unified mouse+keyboard EventQueue (test_mouse_event
#                        exercises it; production input reaches the host via
#                        /dev/event0, not this queue)
#   - gui_init.cpp     : PS/2 mouse + keyboard listener -- dual-writes each
#                        decoded key into /dev/event0 for the userspace host
#   - desktop_launch.cpp: GUI-side launch_userspace (fork+execve /cinux_gui_host)
#                        + handoff_framebuffer_to_gui (§14 interface, non-GUI
#                        counterpart in proc/)
# The host-neutral core (widget tree + Compositor + GuiCore + swraster + region)
# lives at third_party/Cinux-GUI/ and is built as the cinux-gui STATIC lib
# (linked into the userspace host ELF; kernel no longer needs it).
#

# Unified event queue (ring buffer for mouse+keyboard events)
target_sources(big_kernel_common PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/event.cpp
)

# Input init + userspace GUI host launch (host_cinux.cpp deleted in F-GUI-USERSPACE)
target_sources(big_kernel_common PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/gui_init.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/desktop_launch.cpp
)
