#
# kernel/gui/CMakeLists.txt
#
# @brief GUI subsystem CMake configuration
#
# Contains GUI-only components: event queue, window manager, etc.
#

# Event queue (ring buffer for mouse/keyboard events)
target_sources(big_kernel_common PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/event.cpp
)

# Window class (off-screen canvas, title bar, hit testing)
target_sources(big_kernel_common PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/window.cpp
)

# Window manager (Z-ordering, compositing, drag)
target_sources(big_kernel_common PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/window_manager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/window_manager_input.cpp
)

# GUI initialisation (mouse driver, WM setup, PIT tick callback)
target_sources(big_kernel_common PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/gui_init.cpp
)

# Terminal window (80x25 character buffer, cursor, ANSI sequences)
target_sources(big_kernel_common PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/terminal.cpp
)

# F13 cinux-gui: the host-neutral core now lives at third_party/Cinux-GUI/ (built as the
# cinux-gui STATIC lib, linked into the kernel from kernel/CMakeLists.txt).
# kernel/gui/ keeps ONLY the Cinux host adapter -- the single host-specific TU
# that fills the ABI table against the kernel's drivers/framebuffer. The core
# (pump / region / swraseter / abi_check) compiles standalone; see host/ (standalone).
target_sources(big_kernel_common PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/host_cinux.cpp
)
