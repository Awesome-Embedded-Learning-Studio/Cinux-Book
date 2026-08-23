/**
 * @file core/widget/radio.hpp
 * @brief Radio + RadioGroup -- single-choice radio buttons (P7-a)
 *
 * RadioGroup is a NON-Widget manager: it owns the exclusive selection across
 * its Radios. Radio is the Widget (a 16px circle that fills when selected); a
 * press tells its group to select it (which deselects the others). Radios live
 * in any layout container (HBox/VBox); the group pointer wires the exclusion.
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

class Radio;

/** @brief Exclusive-selection manager for a set of Radios (not a Widget). */
class RadioGroup {
public:
    static constexpr uint32_t kMaxRadios = 8;

    void add(Radio* r);
    /** Exclusive select: @p r checked, every other radio in the group cleared. */
    void   select(Radio* r);
    Radio* selected() const { return selected_; }

private:
    Radio*   radios_[kMaxRadios] = {};
    uint32_t count_              = 0;
    Radio*   selected_           = nullptr;
};

class Radio : public Widget {
public:
    void set_group(RadioGroup* g) { group_ = g; }
    void set_theme(const Theme* th) { theme_ = th; }
    bool checked() const { return checked_; }
    /** Group toggles this; invalidates on change. */
    void set_checked(bool c);

    void on_pointer(const PointerPayload& p) override;

protected:
    void paint_to_list(PaintList& list) const override;

private:
    RadioGroup*  group_   = nullptr;
    bool         checked_ = false;
    const Theme* theme_   = nullptr;
};

}  // namespace cinux::gui
