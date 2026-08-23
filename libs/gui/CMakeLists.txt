#
# CMakeLists.txt
#
# @brief Cinux-GUI -- the host-neutral GUI core (the ONLY hard seam to any host)
#
# `cinux-gui` is a STATIC library of pure C++17: stdint/stddef only, ZERO host
# includes. The host adapter (e.g. the Cinux kernel's kernel/gui/host_cinux.cpp)
# is the single host-specific TU and is NOT part of this library -- the core
# never depends on it, only the other way around.
#
# This single file builds the core TWO ways:
#
#   1. As a subdirectory of a parent project (add_subdirectory(third_party/Cinux-GUI)):
#      the parent supplies the toolchain + flags; `cinux-gui` is linked into the
#      host (kernel / SDL / X11 / Wayland). No harness.
#   2. Standalone -- this dir is the build root (`cmake -S . -B build`): it
#      declares its own project under a standard hosted compiler and ALSO builds
#      host/fake_host_main.cpp, a smoke harness that drives cinux::gui::pump()
#      with a hand-filled table (no host, no framebuffer). If that links and
#      runs green, the core is provably host-neutral -- and that harness is the
#      seed of the SDL / X11 / Wayland host adapters.

cmake_minimum_required(VERSION 3.20)

# Declare a project only when this directory IS the build root. As a subdirectory
# the parent project is reused and we stay target-only.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(cinux-gui LANGUAGES C CXX)
    set(CINUX_GUI_STANDALONE ON)
else()
    set(CINUX_GUI_STANDALONE OFF)
endif()

# ============================================================
# host-neutral core library (built in BOTH contexts)
# ============================================================
add_library(cinux-gui STATIC
    core/compositor.cpp
    core/font.cpp
    core/gui_core.cpp
    core/paint_list.cpp
    core/region.cpp
    core/swraster.cpp
    core/theme.cpp
    core/widget.cpp
    core/widget/button.cpp
    core/widget/container.cpp
    core/widget/label.cpp
    core/widget/slider.cpp
    core/widget/window.cpp
    core/widget/window_manager.cpp
    core/widget/desktop_icon.cpp
    core/widget/terminal.cpp
    core/widget/textbox.cpp
    core/widget/checkbox.cpp
    core/widget/radio.cpp
    core/widget/dropdown.cpp
    core/abi_check.cpp
)
# core/ is PUBLIC so consumers (a host adapter, the harness) resolve the bare
# same-dir core includes via the target.
target_include_directories(cinux-gui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/core)
target_compile_features(cinux-gui PUBLIC cxx_std_17)

# ============================================================
# standalone-only: host-neutrality smoke harness
# ============================================================
if(CINUX_GUI_STANDALONE)
    # Zero-warning discipline so the lift set is self-policing; subdirectory
    # builds inherit this from the parent.
    add_compile_options(-Wall -Wextra)
    enable_testing()
    add_executable(cinux-gui-smoke host/fake_host_main.cpp)
    target_link_libraries(cinux-gui-smoke PRIVATE cinux-gui)
    add_test(NAME cinux-gui-smoke COMMAND cinux-gui-smoke)

    # P6-d: offscreen/replay-dump RETIRED (Scene gone; widgets-dump + fbdev-host
    # + window-test/window-manager-test/dirty-test cover the widget-tree path).
    # P1 Probe-1: evdev accumulator unit test (pure logic, no device). IS a ctest.
    add_executable(evdev-test test/test_evdev.cpp host/evdev_device.cpp)
    target_include_directories(evdev-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/host)
    target_link_libraries(evdev-test PRIVATE cinux-gui)
    add_test(NAME evdev-test COMMAND evdev-test)

    # P2-b: Compositor unit test RETIRED in P6-d (Scene gone; execute() is now
    # widget-tree only, covered by widget-test / widgets-test / window-test etc).

    # P1 Probe-1: evdev accumulator unit test (pure logic, no device). IS a ctest.    # P3-a: widget tree unit test (hit-test + dispatch + flatten->PaintList->pixels).
    add_executable(widget-test test/test_widget.cpp)
    target_link_libraries(widget-test PRIVATE cinux-gui)
    add_test(NAME widget-test COMMAND widget-test)

    # P3-b: Material theme + rounded-rect rasteriser.
    add_executable(theme-test test/test_theme.cpp)
    target_link_libraries(theme-test PRIVATE cinux-gui)
    add_test(NAME theme-test COMMAND theme-test)

    # P5-a: font scaling (glyph_blit_scaled) + text measurement.
    add_executable(font-scale-test test/test_font_scale.cpp)
    target_link_libraries(font-scale-test PRIVATE cinux-gui)
    add_test(NAME font-scale-test COMMAND font-scale-test)

    # P6-a: TextBox (keyboard input + cursor) + CheckBox (toggle).
    add_executable(textbox-test test/test_textbox.cpp)
    target_link_libraries(textbox-test PRIVATE cinux-gui)
    add_test(NAME textbox-test COMMAND textbox-test)

    add_executable(checkbox-test test/test_checkbox.cpp)
    target_link_libraries(checkbox-test PRIVATE cinux-gui)
    add_test(NAME checkbox-test COMMAND checkbox-test)

    # P7-a: Radio + RadioGroup (exclusive selection).
    add_executable(radio-test test/test_radio.cpp)
    target_link_libraries(radio-test PRIVATE cinux-gui)
    add_test(NAME radio-test COMMAND radio-test)

    # P7-a: Dropdown (open / select-row / close).
    add_executable(dropdown-test test/test_dropdown.cpp)
    target_link_libraries(dropdown-test PRIVATE cinux-gui)
    add_test(NAME dropdown-test COMMAND dropdown-test)

    # P7-c: Compositor cursor state (set_cursor + render paints a block).
    add_executable(cursor-test test/test_cursor.cpp)
    target_link_libraries(cursor-test PRIVATE cinux-gui)
    add_test(NAME cursor-test COMMAND cursor-test)

    # P6-b: Window resize grip + maximize/minimize.
    add_executable(resize-test test/test_resize.cpp)
    target_link_libraries(resize-test PRIVATE cinux-gui)
    add_test(NAME resize-test COMMAND resize-test)

    # P6-c: terminal ANSI bg + 256 colour + cursor block.
    add_executable(terminal-bg256-test test/test_terminal_bg256.cpp)
    target_link_libraries(terminal-bg256-test PRIVATE cinux-gui)
    add_test(NAME terminal-bg256-test COMMAND terminal-bg256-test)

    # P5-c: per-widget dirty + Desktop::render idle skip.
    add_executable(dirty-test test/test_dirty.cpp)
    target_link_libraries(dirty-test PRIVATE cinux-gui)
    add_test(NAME dirty-test COMMAND dirty-test)

    # P5-d: HBox/VBox flex weights + per-corner rounded rects.
    add_executable(flex-test test/test_flex.cpp)
    target_link_libraries(flex-test PRIVATE cinux-gui)
    add_test(NAME flex-test COMMAND flex-test)

    # P3-c: basic widgets (Label/Button/Container) + HBox/VBox layout.
    add_executable(widgets-test test/test_widgets.cpp)
    target_link_libraries(widgets-test PRIVATE cinux-gui)
    add_test(NAME widgets-test COMMAND widgets-test)

    # P3-d: Slider widget (drag-to-value + press capture).
    add_executable(slider-test test/test_slider.cpp)
    target_link_libraries(slider-test PRIVATE cinux-gui)
    add_test(NAME slider-test COMMAND slider-test)

    # P4-a: Window widget (title bar + drag + close + hit-test).
    add_executable(window-test test/test_window.cpp)
    target_link_libraries(window-test PRIVATE cinux-gui)
    add_test(NAME window-test COMMAND window-test)

    # P4-b: WindowManager (Z-order + raise + close + cursor).
    add_executable(window-manager-test test/test_window_manager.cpp)
    target_link_libraries(window-manager-test PRIVATE cinux-gui)
    add_test(NAME window-manager-test COMMAND window-manager-test)

    # P4-c: TerminalWidget (char grid + write/scroll/paint).
    add_executable(terminal-test test/test_terminal.cpp)
    target_link_libraries(terminal-test PRIVATE cinux-gui)
    add_test(NAME terminal-test COMMAND terminal-test)

    # P5-e: TerminalWidget ANSI SGR colour + cursor + clear.
    add_executable(terminal-ansi-test test/test_terminal_ansi.cpp)
    target_link_libraries(terminal-ansi-test PRIVATE cinux-gui)
    add_test(NAME terminal-ansi-test COMMAND terminal-ansi-test)

    # P4-d: posix_spawn (forkpty) unit test -- host layer, links libutil.
    add_executable(posix-spawn-test test/test_posix_spawn.cpp host/posix_spawn.cpp)
    target_include_directories(posix-spawn-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/host)
    target_link_libraries(posix-spawn-test PRIVATE util)
    add_test(NAME posix-spawn-test COMMAND posix-spawn-test)

    # P3-d: widgets demo host (Material widget tree -> one PPM frame).
    add_executable(widgets-dump host/widgets_host_main.cpp host/ppm_writer.cpp)
    target_link_libraries(widgets-dump PRIVATE cinux-gui)
    add_test(NAME widgets-dump COMMAND widgets-dump)

    # P3 demo: rich Material widget frame (eyeball, not a test).
    add_executable(demo-dump host/demo_host_main.cpp host/ppm_writer.cpp)
    target_link_libraries(demo-dump PRIVATE cinux-gui)

    # P7 gallery: one frame of EVERY widget -> PPM (visual baseline; any host
    # rendering this tree yields identical pixels -- core is host-neutral).
    add_executable(gallery-dump host/gallery_dump_main.cpp host/ppm_writer.cpp)
    target_link_libraries(gallery-dump PRIVATE cinux-gui)

    # P1 Probe-1: real Linux fbdev+evdev host. Manual smoke under QEMU only
    # (needs /dev/fb0 + /dev/input/event*); NOT a ctest.
    add_executable(fbdev-host host/linux_fbdev_main.cpp host/fbdev_device.cpp
                          host/evdev_device.cpp)
    target_link_libraries(fbdev-host PRIVATE cinux-gui)

    # P3 demo: SDL2 interactive host (desktop window; opt-in, needs SDL2).
    # Host layer only -- core never sees SDL.
    option(CINUX_HOST_SDL "Build SDL2 interactive widget host (needs SDL2)" OFF)
    if(CINUX_HOST_SDL)
        find_package(SDL2 REQUIRED)
        add_executable(sdl-host host/sdl_host_main.cpp)
        target_link_libraries(sdl-host PRIVATE cinux-gui SDL2::SDL2)
    endif()

    # P4-d: SDL2 interactive terminal host (real shell via PTY; opt-in).
    # Host layer only -- core never sees SDL or PTY.
    option(CINUX_HOST_TERMINAL "Build SDL2 interactive terminal host (needs SDL2 + PTY)" OFF)
    if(CINUX_HOST_TERMINAL)
        find_package(SDL2 REQUIRED)
        add_executable(terminal-host host/terminal_host_main.cpp host/posix_spawn.cpp)
        target_link_libraries(terminal-host PRIVATE cinux-gui SDL2::SDL2 util)
    endif()

    # P7 gallery: interactive full-widget demo (SDL2; opt-in). Every control
    # live: CheckBox/Radio/Dropdown/Button/Slider/TextBox/Window + keyboard.
    option(CINUX_HOST_GALLERY "Build SDL2 interactive widget gallery (needs SDL2)" OFF)
    if(CINUX_HOST_GALLERY)
        find_package(SDL2 REQUIRED)
        add_executable(gallery-host host/gallery_host_main.cpp)
        target_link_libraries(gallery-host PRIVATE cinux-gui SDL2::SDL2)
    endif()
endif()
