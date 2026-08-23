---
title: 05 · 窗口层:Window + WindowManager + Desktop
---

# 窗口层:Window + WindowManager + Desktop

## Window:复合控件,自画标题栏 + 持一个 content

Window 也是 Widget,但它是个**复合控件**——自己画标题栏 + body,还持一个 content 子控件(终端就挂这儿)。看 [`core/widget/window.hpp`](../../../libs/gui/core/widget/window.hpp#L43-L123):

```cpp
class Window : public Widget {
public:
    static constexpr uint32_t kTitleBarHeight  = 20;
    static constexpr uint32_t kCloseButtonSize = 20;
    static constexpr uint32_t kTitlePadX       = 8;
    static constexpr uint32_t kTitlePadY       = 4;

    void set_title(const char* t) { title_ = t; }
    void set_theme(const Theme* th) { theme_ = th; }
    void set_content(Widget* c);
    Rect content_rect() const;

    void    layout() override;
    Widget* hit_test(int32_t x, int32_t y) override;
    void    on_pointer(const PointerPayload& p) override;
    void    collect_dirty(Region& sink) const override;
    void    clear_dirty() override;

protected:
    void paint_to_list(PaintList& list) const override;

private:
    const char*   title_        = "";
    const Theme*  theme_        = nullptr;
    Widget*       content_      = nullptr;
    CloseCallback on_close_     = nullptr;
    /* 拖拽状态、关闭键 armed 状态、resize 状态... */
};
```

`set_content` 把一个子控件挂进 content 槽,同时调 `add_child` 把它加进 Widget 框架的 `children_` 数组(这样 `flatten` 会递归到它):

```cpp
void Window::set_content(Widget* c) {
    if (c == nullptr || content_ != nullptr) {
        return;                          // null 或已设 -> 忽略(单 content 槽)
    }
    content_ = c;
    add_child(c);
}
```

`layout` 把 content 的 rect 设成"标题栏下方的区域"——这是 Window 跟 TerminalWidget 解耦的关键:TerminalWidget 不需要知道"我外面是标题栏还是别的什么",它只看 `rect_`、以为自己的 rect 就是世界;Window 负责把它放到 content 区:

```cpp
void Window::layout() {
    if (content_ == nullptr) {
        return;
    }
    const Rect cr = content_rect();
    content_->set_rect(cr.x0, cr.y0, cr.width(), cr.height());
    content_->layout();
}
```

`paint_to_list` 自画标题栏:先一个圆角 body(`fill_round_rect`)、再一个标题色带(`fill_round_rect_corners` 只圆顶角)、标题文本(`text`)、右上角关闭键 "x"。这部分跟 030 的窗口骨架是一脉相承的,差别只在用的是 PaintList 指令而不是 Canvas 的 `draw_*`。

`hit_test` 是自定义的,因为 Window 有好几个命中区:关闭键、resize grip、标题栏(可拖)、content 区。看 [`window.cpp`](../../../libs/gui/core/widget/window.cpp#L77-L96)——关闭键和 resize grip 命中返 `this`(Window 自己处理);标题栏命中也返 `this`(开始拖);content 区命中则递归 `content_->hit_test`(让子控件接住);空 content 区回退到 `this`。这就是复合控件的命中语义:父控件可以选择"这事我自己来"或者"让我的 content 处理"。

`on_pointer` 处理拖拽:down 在标题栏记 drag 起点、move 算 delta 调 `move_to_`(改 rect + relayout + 标脏 old + new)、up 清状态。这里有个保留模式特有的细节——**移动窗口要标 old footprint 脏**:

```cpp
void Window::move_to_(int32_t x, int32_t y) {
    const uint32_t w   = rect_.width();
    const uint32_t h   = rect_.height();
    const Rect     old = rect_;
    set_rect(x, y, w, h);
    layout();
    invalidate(old);          // 旧位置要重画(露背景)
    invalidate();             // 新位置也要重画
}
```

窗口移走了,旧位置那块矩形原来盖着的是别的窗口或桌面背景,现在露出来了——这块必须标脏,否则合成器不会重画它,屏幕上会留下窗口的"残影"。即时模式下全屏重画自动解决;保留模式必须显式标 old。这是从即时模式切到保留模式最容易漏的点。

## WindowManager:桌面根,自管 windows_ 数组

WindowManager 也是 Widget,作桌面根。但它**不用 Widget 框架的 `children_`**——它自己管一个 `windows_[]` 数组。原因在 [`window_manager.hpp`](../../../libs/gui/core/widget/window_manager.hpp#L11-L20) 的头注释里说得很直白:框架的 `flatten` 是 self→children 顺序,无法表达"光标画在所有窗口之上"(`paint_to_list` 在 children 之前跑)。所以 WM 自管数组、在 `paint_to_list` 里手动按正确顺序画:

```cpp
void WindowManager::paint_to_list(PaintList& list) const {
    const uint32_t bg = (theme_ != nullptr) ? theme_->background : 0x00000000u;
    list.fill_rect(rect_.x0, rect_.y0, rect_.width(), rect_.height(), bg);   // 1. 桌面背景

    for (uint32_t i = 0u; i < icon_count_; ++i) {                            // 2. 桌面图标(bg-level)
        icons_[i]->flatten(list);
    }

    for (uint32_t i = 0; i < count_; ++i) {                                  // 3. 窗口底→顶
        windows_[i]->flatten(list);
    }
    /* 鼠标光标不在这里画——由 Compositor::render 末尾画 */
}
```

顺序是 bg → 图标 → 窗口(底到顶),保证后加的窗口画在前面、图标可以被窗口遮住。**鼠标指针不在这张清单里**——它由 [`Compositor::render`](../../../libs/gui/core/compositor.cpp#L116-L186) 在跑完整张 cmd 清单**之后**补画:一张硬编码的 16×16 单色箭头位图(legacy CinuxOS 的指针资源),每个亮像素画成 1px 灰体 + 1px 白描边,坐标从 `set_cursor(cx, cy, has_cursor)` 拿(`Desktop::render` 调 `root->cursor_pos` 问 WM 鼠标在哪)。这样光标永远画在所有窗口之上、不用塞进 paint_to_list;连带一个副作用——光标的 footprint 必须由 WM 自己 union 进脏区(`process_pointer` 里 `invalidate(old cursor footprint) + invalidate(new)`,见上节"光标拖影"那条判据),否则它不在脏区里、`render` 的 outer clip 会把它跳过。

这个"自管数组、不用 children_"的决定带来一个连带义务:**`collect_dirty` 和 `clear_dirty` 都要 override,显式递归 `windows_[]`**。框架默认的 `Widget::collect_dirty` 只递归 `children_`,而 WM 的 `children_` 是空的——不 override 的话,Window 和它的 content TerminalWidget 的脏区永远到不了 root,屏幕就不更新。看 [`window_manager.cpp`](../../../libs/gui/core/widget/window_manager.cpp#L159-L191):

```cpp
void WindowManager::collect_dirty(Region& sink) const {
    if (dirty_self_) {
        sink.add(dirty_rect_);
    }
    for (uint32_t i = 0u; i < icon_count_; ++i) {
        icons_[i]->collect_dirty(sink);
    }
    for (uint32_t i = 0u; i < count_; ++i) {
        windows_[i]->collect_dirty(sink);          // 显式递归 windows_(不在 children_)
    }
}

void WindowManager::clear_dirty() {
    dirty_self_ = false;
    dirty_rect_ = Rect{1, 1, 0, 0};
    for (uint32_t i = 0u; i < icon_count_; ++i) {
        icons_[i]->clear_dirty();
    }
    for (uint32_t i = 0u; i < count_; ++i) {
        windows_[i]->clear_dirty();                // 镜像 collect_dirty
    }
}
```

`process_pointer` 是 WM 的事件入口,自带 press capture(拖拽用):down 时 hit-test 找到目标窗口、`raise` 把它顶到最前(click-to-raise)、投事件给它、记 press_target;move 时投给 press_target(保持拖拽即使光标离开窗口);up 时投给 press_target(可能触发 on_close → remove_window)然后清掉。键盘事件不走 WM——`Desktop::dispatch_key` 把它投给 focus widget(就是上次 click 命中的那个)。

## Desktop:把树驱动起来

最后是把这一切串起来的 [`Desktop`](../../../libs/gui/core/widget.hpp#L147-L172)。它持根指针、一个 Compositor、一张 PaintList,驱动 `dispatch_pointer` / `dispatch_key` / `render`:

```cpp
void Desktop::render(Surface& staging, const PsfFont& font, Region* dirty) {
    Region  local;
    Region* d = (dirty != nullptr) ? dirty : &local;
    d->clear();
    if (root_ == nullptr) {
        return;
    }
    const Rect full{0, 0, static_cast<int32_t>(staging.width),
                    static_cast<int32_t>(staging.height)};
    if (first_) {
        d->add(full);                       // 首帧全屏
        first_ = false;
    } else {
        root_->collect_dirty(*d);           // 收 per-widget 脏区
    }
    if (d->empty()) {
        return;                             // idle -> 0 rects -> pump flushes nothing
    }
    root_->clear_dirty();
    root_->layout();
    paint_list_.clear();
    root_->flatten(paint_list_);            // 整树拍成清单(一次)
    int32_t    cx = 0;
    int32_t    cy = 0;
    const bool has_cursor = root_->cursor_pos(&cx, &cy);
    comp_.set_cursor(cx, cy, has_cursor);
    const uint32_t n = d->count();
    for (uint32_t i = 0u; i < n; ++i) {     // 对每个脏区,clip 着重画整张清单(幂等)
        const Rect&    r = d->rects()[i];
        const ClipRect clip{r.x0, r.y0, r.x1, r.y1};
        comp_.render(staging, paint_list_, font, &clip);
    }
}
```

这里有一个关键决定:**flatten 一次,execute 多次(每个脏区一次)**。N 个脏区 → N 次 `comp_.render`,每次都跑完整张 `paint_list_`,但用脏区作 clip——clip 外的 cmd 跳过,实际只在脏区内画。这看着浪费(同样的 cmd 跑 N 遍),但 N 通常很小(一行 shell 输出 + 光标块 = 2~3 个脏区),而 cmd 在 clip 外的跳过是 O(1) 的快速路径,总开销远小于"为每个脏区重新 flatten"。这是用幂等换简单——同一个 cmd 在不同 clip 下都正确,不用记"哪条 cmd 属于哪个脏区"。

`first_` 标志首帧全屏——因为 Widget 构造时 `dirty_self_=true` 但 `dirty_rect_` 是 degenerate(空),`collect_dirty` 首帧 add 个空 rect 等于 idle,屏幕就什么也不画。`first_` 强制首帧把全屏加进脏区,绕过这个冷启动陷阱。

