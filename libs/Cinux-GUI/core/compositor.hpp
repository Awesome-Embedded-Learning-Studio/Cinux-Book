/**
 * @file core/compositor.hpp
 * @brief cinux::gui Compositor -- paints a widget tree's PaintList into staging
 *
 * The host-neutral renderer. render() walks a flattened PaintList (from a
 * Widget tree) and dispatches each cmd to a registered handler (CmdKind -> fn).
 *
 * Adding a shape is OPEN: a new CmdKind + PaintCmd field + swraster primitive +
 * set_handler() -- render() itself never changes. That is the whole point of the
 * handler table (vs a switch in render): the compositor stops being the place
 * every new primitive has to touch. kClipPush/kClipPop stay internal because
 * they drive the clip stack, not a drawable primitive.
 *
 * P2's Scene-based Compositor (compose(Scene), frame-to-frame dirty diff) was
 * retired in P6-d: the widget tree + per-widget dirty (P5-f) replaced it.
 *
 * It is a CLASS so the handler table + future state (cursor footprint, frame
 * diff, GPU path) have an owner, and adding state doesn't churn callers.
 *
 * Zero host includes; depends only on paint_list/font/swraster (all core/).
 *
 * Compile condition: CINUX_GUI.
 * Namespace: cinux::gui
 */
#pragma once

#include "font.hpp"        // PsfFont
#include "paint_list.hpp"  // PaintList, PaintCmd, CmdKind
#include "swraster.hpp"    // Surface, ClipRect

namespace cinux::gui {

/**
 * @brief Host-neutral compositor: PaintList -> staging pixels
 *
 * Stateless today (the widget tree + per-widget dirty own the dirty side); the
 * handler table is the extension point for new primitives, the class shape is
 * the extension point for future state.
 */
class Compositor {
public:
    /** Draw one PaintCmd into @p s under @p clip (@p font for text cmds). */
    using Handler = void (*)(Surface& s, const PaintCmd& cmd, const PsfFont& font,
                             const ClipRect* clip);

    /** Registers the default primitives: fill_rect / fill_round_rect / text /
     * text_glyph / text_scaled. (Clip push/pop are handled in render itself.) */
    Compositor();

    /** Register or override a primitive handler. Adding a shape is OCP: this,
     * not a change to render(). */
    void set_handler(CmdKind kind, Handler h);

    /**
     * @brief Paint @p list into @p staging
     *
     * Walks @p list in order, dispatching each cmd to its handler. kClipPush
     * intersects a new rect with the current top; kClipPop pops. So a widget can
     * never paint outside its ancestors' rects.
     *
     * @param staging  core-owned staging Surface
     * @param list     flattened draw cmds (from Widget::flatten)
     * @param font     PSF2 font (passed to handlers; text cmds need it)
     * @param outer    optional base clip (a dirty rect limits the repaint to it)
     */
    void render(Surface& staging, const PaintList& list, const PsfFont& font,
                const ClipRect* outer = nullptr);

    /** P7-c: cursor state. render() paints a 4x4 block at (x,y) when visible. */
    void set_cursor(int32_t x, int32_t y, bool visible);

private:
    static constexpr uint32_t kKindCount            = 7u;  // kFillRect..kClipPop (see CmdKind)
    Handler                   handlers_[kKindCount] = {};
    int32_t                   cursor_x_       = 0;  // P7-c: cursor footprint (state in the class)
    int32_t                   cursor_y_       = 0;
    bool                      cursor_visible_ = false;
};

}  // namespace cinux::gui
