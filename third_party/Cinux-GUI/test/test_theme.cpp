/**
 * @file test/test_theme.cpp
 * @brief Material theme + rounded-rect rasteriser (P3-b)
 *
 * Checks material_light/dark return the documented Material palette, and
 * fill_rounded_rect paints a disc-ish shape: centre + edge-mid are the colour,
 * the four outer corners stay background (the arc bites in), radius==0 falls
 * back to a plain square fill_rect, and an over-large radius is clamped.
 */

#include <cstdint>
#include <cstdio>

#include "swraster.hpp"
#include "theme.hpp"

using namespace cinux::gui;

#define CHECK(cond, ...)                       \
    do {                                       \
        if (!(cond)) {                         \
            std::printf("FAIL: " __VA_ARGS__); \
            std::printf("\n");                 \
            return 1;                          \
        }                                      \
    } while (0)

namespace {

constexpr uint32_t kW = 320, kH = 240;

struct Stage {
    uint8_t* buf;
    Surface  s;
    Stage() : buf(new uint8_t[kW * kH * 4u]()), s{buf, kW, kH, kW * 4u, PixelFormat::kXrgb8888} {}
    ~Stage() { delete[] buf; }
    const uint32_t* px() const { return reinterpret_cast<const uint32_t*>(buf); }
};

}  // namespace

int main() {
    // 1. Material light palette (documented Material baseline colours)
    {
        Theme t = material_light();
        CHECK(t.primary == 0x006200EEu, "light primary 0x%08X", t.primary);
        CHECK(t.on_primary == 0x00FFFFFFu, "light on_primary 0x%08X", t.on_primary);
        CHECK(t.background == 0x00F5F5F5u, "light background 0x%08X", t.background);
        CHECK(t.button_radius == kRadiusMedium, "button radius %u", t.button_radius);
        CHECK(t.card_radius == kRadiusLarge, "card radius %u", t.card_radius);
    }

    // 2. Material dark palette
    {
        Theme t = material_dark();
        CHECK(t.primary == 0x00BB86FCu, "dark primary 0x%08X", t.primary);
        CHECK(t.on_surface == 0x00FFFFFFu, "dark on_surface 0x%08X", t.on_surface);
    }

    // 3. rounded rect: centre + edge-mids painted, corners stay background
    {
        Stage          st;
        const uint32_t red = 0x00FF0000u;
        fill_rounded_rect(st.s, 10, 10, 20, 20, red, 6, nullptr);
        const uint32_t* px = st.px();
        CHECK(px[20 * kW + 20] == red, "centre should be red");
        CHECK(px[10 * kW + 10] == 0u, "outer corner should stay bg (arc bites in)");
        CHECK(px[20 * kW + 10] == red, "top-edge mid should be red");
        CHECK(px[10 * kW + 20] == red, "left-edge mid should be red");
    }

    // 4. radius == 0 == plain fill_rect (square corners painted)
    {
        Stage          st;
        const uint32_t red = 0x00FF0000u;
        fill_rounded_rect(st.s, 10, 10, 20, 20, red, 0, nullptr);
        const uint32_t* px = st.px();
        CHECK(px[10 * kW + 10] == red, "square corner should be red (radius 0)");
        CHECK(px[29 * kW + 29] == red, "square far corner should be red");
    }

    // 5. over-large radius is clamped to min(w,h)/2 (no crash, still painted)
    {
        Stage st;
        fill_rounded_rect(st.s, 10, 10, 6, 6, 0x00FF0000u, 100, nullptr);  // -> radius 3
        const uint32_t* px = st.px();
        CHECK(px[13 * kW + 13] == 0x00FF0000u, "tiny rounded centre red");
    }

    // 6. P5-b: container / variant colours are set and differ light vs dark
    {
        Theme tl = material_light();
        Theme td = material_dark();
        CHECK(tl.primary_container != 0u, "light primary_container set");
        CHECK(tl.on_primary_container != tl.primary_container, "container has contrast");
        CHECK(tl.surface_variant != tl.surface, "light surface_variant != surface");
        CHECK(td.primary_container != tl.primary_container, "light/dark container differ");
        CHECK(td.surface_variant != tl.surface_variant, "light/dark surface_variant differ");
    }

    std::printf("theme-test: OK (palette/rounded-corners/radius0/clamp/variants)\n");
    return 0;
}
