/**
 * @file core/widget.hpp
 * @brief cinux::gui widget tree -- host-neutral control layer (P3)
 *
 * A Widget owns a rectangle, optional children, and three virtual hooks:
 *   - paint_to_list: append this widget's draw cmds (clip already set to rect)
 *   - hit_test:      which widget a pointer at (x,y) targets (default: rect)
 *   - on_pointer:    receive a pointer event routed to this widget
 *
 * Desktop drives the tree: dispatch_pointer hit-tests + delivers (no bubbling,
 * per P3 decision C); render flattens to a PaintList + executes it. P3-a is the
 * skeleton + routing; Label/Button/Container/Slider arrive in P3-c/d.
 *
 * Pure C++17 (stdint/stddef + core/ only), ZERO host includes. Virtual dispatch
 * is fine (not RTTI, which is banned); children are a fixed array (no <vector>).
 *
 * Compile condition: CINUX_GUI.
 * Namespace: cinux::gui
 */
#pragma once

#include <stdint.h>

#include "compositor.hpp"     // Compositor (Desktop owns one, P7-c)
#include "event_payload.hpp"  // PointerPayload / KeycodePayload
#include "paint_list.hpp"     // PaintList
#include "region.hpp"         // Rect

namespace cinux::gui {

struct Surface;  // forward (swraster.hpp) -- render takes a reference
class PsfFont;   // forward (font.hpp)

/**
 * @brief Base widget: rectangle + children + virtual paint/hit/event hooks
 *
 * Subclass to make a real control (Label/Button/...). The framework (flatten,
 * hit_test) handles clip + recursion; subclasses only implement the protected
 * paint_to_list and the public on_pointer / hit_test overrides they need.
 */
class Widget {
public:
    static constexpr uint32_t kMaxChildren = 16;

    virtual ~Widget() = default;

    void set_rect(int32_t x, int32_t y, uint32_t w, uint32_t h);
    Rect rect() const { return rect_; }
    void set_visible(bool v) { visible_ = v; }
    bool visible() const { return visible_; }

    /** Flex weight for HBox/VBox layout (P5-d). Default 1 = equal share. */
    void     set_flex(uint32_t f) { flex_ = f; }
    uint32_t flex() const { return flex_; }

    void     add_child(Widget* w);
    uint32_t child_count() const { return child_count_; }
    Widget*  child(uint32_t i) const { return i < child_count_ ? children_[i] : nullptr; }

    /**
     * @brief Framework paint: clip to this rect, paint self, recurse children
     *
     * Non-virtual -- the framework entry. Subclasses override the protected
     * paint_to_list for their own drawing; flatten handles clip push/pop and
     * recursion so a widget can never paint outside its ancestors' rects.
     */
    void flatten(PaintList& list) const;

    /**
     * @brief Find the widget a pointer at (x,y) targets (topmost first)
     *
     * Default recurses children last-to-first (later children paint on top),
     * then this widget if visible and contains the point. Override for
     * non-rectangular hit shapes (e.g. a round button).
     */
    virtual Widget* hit_test(int32_t x, int32_t y);

    /** Receive a pointer event routed to this widget. Default: noop. */
    virtual void on_pointer(const PointerPayload& p) { (void)p; }
    /** Receive a keyboard event routed to this widget (P6-a). Default: noop. */
    virtual void on_key(const KeycodePayload& k) { (void)k; }
    /** P7-c: expose a cursor position for the Compositor to paint. Default: none
     * (returns false). WindowManager overrides (it tracks the cursor). */
    virtual bool cursor_pos(int32_t* x, int32_t* y) const {
        (void)x;
        (void)y;
        return false;
    }

    /**
     * @brief Recompute children's rects (containers override; P3-c)
     *
     * Default noop. Containers (HBox/VBox) position their children within this
     * rect and call layout() on each so nested containers settle. Desktop::render
     * calls root->layout() before paint.
     */
    virtual void layout() {}

    /** Mark this widget's own rect dirty (call on visible state change). */
    void invalidate() { invalidate(rect_); }
    /** Mark an arbitrary rect dirty (P5-f: cursor/move footprints, term rows). */
    void invalidate(Rect r) {
        dirty_self_ = true;
        dirty_rect_ = rect_union(dirty_rect_, r);
    }
    /** Append this widget's dirty rect (if any) + recurse children. P5-f. */
    virtual void collect_dirty(Region& sink) const {
        if (dirty_self_) {
            sink.add(dirty_rect_);
        }
        for (uint32_t i = 0u; i < child_count_; ++i) {
            children_[i]->collect_dirty(sink);
        }
    }
    /** Clear dirty on this widget and all descendants. */
    virtual void clear_dirty() {
        dirty_self_ = false;
        dirty_rect_ = Rect{1, 1, 0, 0};  // degenerate (empty)
        for (uint32_t i = 0u; i < child_count_; ++i) {
            children_[i]->clear_dirty();
        }
    }

protected:
    /** Subclass drawing (clip already set to this widget's rect). Default: noop. */
    virtual void paint_to_list(PaintList& list) const { (void)list; }

    Rect     rect_{};
    bool     visible_                = true;
    Widget*  children_[kMaxChildren] = {};
    uint32_t child_count_            = 0u;
    Widget*  parent_                 = nullptr;  // set by add_child
    bool     dirty_self_             = true;     // P5-f: start dirty (frame 1 via Desktop::first_)
    Rect     dirty_rect_             = Rect{1, 1, 0, 0};  // P5-f: accumulated dirty
    uint32_t flex_                   = 1u;                // P5-d: HBox/VBox weight (1 = equal)
};

/**
 * @brief Desktop: owns the widget tree root, drives dispatch + render
 *
 * dispatch_pointer hit-tests from the root and delivers the event to the
 * target (no bubbling). render flattens the tree to a PaintList and executes
 * it into the staging Surface. P3-a always reports a full-screen dirty Region
 * (the tree repaints wholesale each frame); per-widget dirty flags arrive
 * later.
 */
class Desktop {
public:
    void    set_root(Widget* root) { root_ = root; }
    Widget* root() const { return root_; }

    /** Hit-test + deliver @p p to the target widget (no event bubbling). */
    void dispatch_pointer(const PointerPayload& p);
    /** Deliver @p k to the focused widget (P6-a). Click sets focus (pointer down). */
    void dispatch_key(const KeycodePayload& k);

    /**
     * @brief Flatten the tree + paint into @p staging
     *
     * @p dirty (optional) receives the changed Region. P3-a reports the full
     * screen every frame; tighter dirty tracking arrives with widget dirty flags.
     */
    void render(Surface& staging, const PsfFont& font, Region* dirty);

private:
    Widget*    root_         = nullptr;
    Widget*    press_target_ = nullptr;  // P3-d: press capture (drag tracks the press widget)
    Widget*    focus_        = nullptr;  // P6-a: keyboard focus (set on pointer down)
    bool       first_        = true;     // P5-f: paint full screen on frame 1
    Compositor comp_;                    // P7-c: owns cursor state + paint handlers
    PaintList   paint_list_;                  // F13-B: PaintCmd[4096]~128KB owned flyweight (kernel 8KB stack safe)                    // P7-c: owns cursor state + paint handlers
};

}  // namespace cinux::gui
