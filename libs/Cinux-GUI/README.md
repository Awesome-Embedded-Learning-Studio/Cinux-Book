# Cinux-GUI

Host-neutral GUI core + PC host framework for [CinuxOS](https://github.com/Awesome-Embedded-Learning-Studio/CinuxOS).
One core, many hosts: the same `cinux::gui::pump()` drives the Cinux kernel today
and an SDL / X11 / Wayland host tomorrow — swap the host-ABI table fill, the core
is unchanged.

## Layout

```
core/      host-neutral core (stdint/stddef only, ZERO host includes)
           cgui_host.h     -- the ONLY hard seam: a function-pointer table (Host ABI)
           cgui_event*.h   -- fixed-width event header + payload (cross-privilege stable)
           cgui_pump.*     -- cinux::gui::pump(): drain input -> render frame -> flush dirty rects
           cgui_region.*   -- cinux::gui::Rect + bounded Region (dirty-region algebra)
           cgui_swraseter.*-- pure-CPU integer drawing primitives (Q8.8 blend, 1-bpp glyph mask)
           cgui_abi_check  -- compile-time ABI self-check (header sizes / packed-ness)
host/      host adapters (swap the table fill = swap the host)
           fake_host_main.cpp -- zero-kernel neutrality proof + SDL/MCU adapter seed
```

## Build (standalone)

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure   # cgui_host_smoke: null-host/idle-skip/dirty-flush/region
```

The same `CMakeLists.txt` also builds as a **subdirectory** of a parent project
(e.g. the Cinux kernel vendors it) — guarded by `CMAKE_SOURCE_DIR`, it then
provides only the `cgui_core` STATIC lib without the harness.

## Status

Core is host-neutral and standalone-build-proven. **TODO**: real PC host adapters
(`host/sdl`, `host/x11`, `host/wayland`) + the widget/compositor body (M0-M9).

## Origin

Seeded from CinuxOS milestone F13 (visor → `cgui` rename). Full provenance is in
the CinuxOS history; this repo starts from the renamed, separated core.
