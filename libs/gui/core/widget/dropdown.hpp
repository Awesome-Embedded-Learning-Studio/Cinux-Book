/**
 * @file core/widget/dropdown.hpp
 * @brief Dropdown -- closed shows the selected option; open shows the list (P7-a)
 *
 * A click opens the list (paints every option row); a click on a row selects it
 * and closes. The widget rect is the FULL list height (rows * kRowH), so closed
 * paints just the top (selected) row + a "v" marker and the rest as surface bg.
 * That keeps hit-testing inside the rect (no popup outside the widget model).
 *
 * Pure C++17 (stdint/stddef + core/ only), ZERO host includes.
 * Compile condition: CINUX_GUI.
 */
#pragma once

#include <stdint.h>

#include "../event_payload.hpp"  // PointerPayload
#include "../paint_list.hpp"     // PaintList
#include "../theme.hpp"          // Theme
#include "../widget.hpp"         // Widget

namespace cinux::gui {

class Dropdown : public Widget {
public:
    static constexpr uint32_t kMaxOpts = 8;
    static constexpr uint32_t kRowH    = 20;  // set rect height = opt_count * kRowH

    void set_option(uint32_t i, const char* text);
    void set_option_count(uint32_t n);
    void set_theme(const Theme* th) { theme_ = th; }

    uint32_t    selected() const { return selected_; }
    const char* selected_text() const;
    bool        expanded() const { return expanded_; }

    void on_pointer(const PointerPayload& p) override;

protected:
    void    paint_to_list(PaintList& list) const override;
    Widget* hit_test(int32_t x, int32_t y) override;

private:
    uint32_t row_at_(int32_t y) const;  // y -> row index (may be >= opt_count_)

    const char*  options_[kMaxOpts] = {};
    uint32_t     opt_count_         = 0;
    uint32_t     selected_          = 0;
    bool         expanded_          = false;
    const Theme* theme_             = nullptr;
};

}  // namespace cinux::gui
