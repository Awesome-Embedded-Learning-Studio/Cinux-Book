/**
 * @file core/widget/container.hpp
 * @brief Container + HBox + VBox -- layout containers (P3-c)
 *
 * Container: a Widget with an optional solid background (children paint on
 * top, inherited from Widget::flatten). HBox/VBox override layout() to share
 * content rect among children equally along their axis with a fixed gap.
 *
 * Layout is integer-only and divide-evenly (no flex weights, no
 * preferred-size negotiation -- P3 decision C: linear first, flex later).
 *
 * Compile condition: CINUX_GUI.
 * Namespace: cinux::gui
 */
#pragma once

#include <stdint.h>

#include "../paint_list.hpp"  // PaintList
#include "../widget.hpp"      // Widget

namespace cinux::gui {

class Container : public Widget {
public:
    /** Background fill colour; 0 = transparent. */
    void set_bg(uint32_t bg) { bg_ = bg; }

protected:
    void paint_to_list(PaintList& list) const override;

    uint32_t bg_ = 0u;
};

/** Horizontal layout: children share width equally, left-to-right, with gap. */
class HBox : public Container {
public:
    void set_spacing(uint32_t s) { spacing_ = s; }
    void set_padding(uint32_t p) { padding_ = p; }
    void layout() override;

private:
    uint32_t spacing_ = 0u;
    uint32_t padding_ = 0u;
};

/** Vertical layout: children share height equally, top-to-bottom, with gap. */
class VBox : public Container {
public:
    void set_spacing(uint32_t s) { spacing_ = s; }
    void set_padding(uint32_t p) { padding_ = p; }
    void layout() override;

private:
    uint32_t spacing_ = 0u;
    uint32_t padding_ = 0u;
};

}  // namespace cinux::gui
