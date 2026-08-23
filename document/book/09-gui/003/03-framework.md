---
title: 03 · 渲染框架:Widget + PaintList + Compositor
---

# 渲染框架:Widget + PaintList + Compositor

## Widget 基类:三个虚 hook + 套娃树

要做"窗口里能放不同内容",所有能画、能命中、能收事件的东西得共享一个虚接口。这就是 [`core/widget.hpp`](../../../libs/gui/core/widget.hpp#L41-L136) 的 `Widget`。它的核心是三个虚 hook:

```cpp
class Widget {
public:
    static constexpr uint32_t kMaxChildren = 16;

    virtual ~Widget() = default;

    void set_rect(int32_t x, int32_t y, uint32_t w, uint32_t h);
    Rect rect() const { return rect_; }
    void set_visible(bool v) { visible_ = v; }

    void    add_child(Widget* w);
    uint32_t child_count() const { return child_count_; }
    Widget*  child(uint32_t i) const { return i < child_count_ ? children_[i] : nullptr; }

    /* 框架入口(非虚):clip 到自己 rect → paint_to_list(虚) → 递归 child → clip pop */
    void flatten(PaintList& list) const;

    virtual Widget* hit_test(int32_t x, int32_t y);
    virtual void on_pointer(const PointerPayload& p) { (void)p; }
    virtual void on_key(const KeycodePayload& k) { (void)k; }

    virtual void layout() {}

    void invalidate() { invalidate(rect_); }
    void invalidate(Rect r);
    virtual void collect_dirty(Region& sink) const;
    virtual void clear_dirty();

protected:
    /* 子类的画法(clip 已被框架 push 到自己 rect)。默认 noop。 */
    virtual void paint_to_list(PaintList& list) const { (void)list; }

    Rect     rect_{};
    bool     visible_                = true;
    Widget*  children_[kMaxChildren] = {};
    uint32_t child_count_            = 0u;
    Widget*  parent_                 = nullptr;
    bool     dirty_self_             = true;
    Rect     dirty_rect_             = Rect{1, 1, 0, 0};
    uint32_t flex_                   = 1u;
};
```

这里有几个设计决定值得点破。

**`flatten` 是非虚的框架入口,`paint_to_list` 才是子类填的虚 hook。** 这样 clip push/pop 和递归子控件由框架统一管,子类只管"画我自己",不用操心"我的祖先矩形是啥""我的孩子要不要递归"。看 [`core/widget.cpp`](../../../libs/gui/core/widget.cpp#L33-L43) 的实现就一目了然:

```cpp
void Widget::flatten(PaintList& list) const {
    if (!visible_) {
        return;
    }
    list.clip_push(rect_.x0, rect_.y0, rect_.x1, rect_.y1);
    paint_to_list(list);  // virtual -- subclass drawing
    for (uint32_t i = 0u; i < child_count_; i++) {
        children_[i]->flatten(list);
    }
    list.clip_pop();
}
```

`clip_push` 把自己的矩形推进合成器的裁剪栈,`clip_pop` 弹出——合成器执行 cmd 时,每条 `fill_rect`/`text_glyph` 都会跟栈顶矩形求交,超出部分直接跳过。这就是"控件画不出祖先矩形外"的双层防御之一(另一层是 `collect_dirty` 只收自己的脏区)。

**`hit_test` 默认按"子控件后画的在上、先命中"递归。** 看 [`widget.cpp`](../../../libs/gui/core/widget.cpp#L45-L56):children 从后往前(last-to-first)递归,因为后 add 的 child 画在上面、应该先被点中;都不命中才轮到自己。子类可以 override 成非矩形的命中形状(比如圆角按钮),或者像 Window 那样自定义命中逻辑(标题栏、关闭键、内容区各走各的)。

**`on_pointer`/`on_key` 默认 noop。** 老的、不需要响应输入的控件照样能跑;新控件 override 掉就自动接管了对应事件。注意键盘事件 `on_key` 收的是 [`KeycodePayload`](../../../libs/gui/core/event_payload.hpp#L41-L45)(ascii + scancode + modifiers 三字节),不是 030 那种 `KeyEvent`——这是因为事件要跨进程传输(087 的 `/dev/event0` 走的就是这套 wire layout),payload 必须 packed、定长。

**`invalidate` 是保留模式的"标脏"入口。** 控件改了状态(写了字、按了按钮、拖了窗口),不主动画,只调 `invalidate(Rect)` 把那块矩形 union 进自己的 `dirty_rect_`。真正的画在帧边界由根统一做。`dirty_self_` 初值是 `true`——控件构造时还没画过,首帧必画;之后 `clear_dirty` 把它清掉,空闲时就不再重画(idle → 0 flush)。

这一步看着是个基类,却是整个应用层的地基。没有这套虚接口,WindowManager 就没法"不认识 TerminalWidget 却能把事件送进去、把它的绘制指令收上来"。代价是每个 Widget 多一个 vptr——这点开销可以忽略,换来的解耦值回票价。

## PaintList:绘制指令的有序清单

控件不直接画像素了,那它产出的"我想画什么"住哪儿?就是 [`core/paint_list.hpp`](../../../libs/gui/core/paint_list.hpp#L109-L138) 的 `PaintList`——一张定长的绘制指令数组:

```cpp
class PaintList {
public:
    static constexpr uint32_t kMaxCmds = 4096;

    void clear() { count_ = 0u; }
    uint32_t count() const { return count_; }

    void fill_rect(int32_t x, int32_t y, uint32_t w, uint32_t h, uint32_t color);
    void fill_round_rect(int32_t x, int32_t y, uint32_t w, uint32_t h, uint32_t color,
                         uint32_t radius);
    void fill_round_rect_corners(int32_t x, int32_t y, uint32_t w, uint32_t h, uint32_t color,
                                 uint32_t radius, uint32_t corners);
    void text(int32_t x, int32_t y, uint32_t color, const char* str);
    void text_glyph(int32_t x, int32_t y, uint32_t color, char ch);
    void text_scaled(int32_t x, int32_t y, uint32_t color, const char* str, uint32_t scale);
    void clip_push(int32_t x0, int32_t y0, int32_t x1, int32_t y1);
    void clip_pop();

private:
    PaintCmd cmds_[kMaxCmds];
    uint32_t count_ = 0u;
};
```

每条指令是一个 `PaintCmd`——`CmdKind` 标种类(填充 / 圆角填充 / 文本 / 单字符 / 缩放文本 / clip push / clip pop),加一个匿名 union 装该种指令的 POD 载荷。比如 `text_glyph` 的载荷就是 `TextGlyphCmd{x, y, color, ch}`,`ch` 是**内联进 cmd** 的单个 char,不借任何外部指针。

这里有个设计细节值得专门讲:**为什么单字符要单独搞一个 `kTextGlyph` 指令,不复用 `kText`?** 看头注释给出的理由——`kText` 的载荷是个 `const char*`,**借用调用方的指针、当帧有效**;可终端要逐字符渲染,如果用 `kText`,就得给每个 cell 准备一个稳定的 `char[2]` 缓冲(字符 + 结尾 `\0`)。这缓冲要是 paint_to_list 的栈临时,函数一返回就失效,合成器后续 execute 时读到的就是 dangling 指针。`kTextGlyph` 把字符直接内联进 cmd(一个 `char ch` 字段,无借用),从根上规避了这个问题。per-cell 字符渲染无需任何持久缓冲。这是字符密集型控件最容易踩的坑,源码用专门的指令类型把它堵死了。

容量 `kMaxCmds = 4096` 也不是随便取的。头注释明说:一个满载的 80×25 终端 = 2000 条 `text_glyph` cmd(每个非空 cell 一条),加上周围树(窗口标题栏、关闭键、桌面图标……),256 远不够;4096 条 `sizeof(PaintCmd)≈32`,整张清单约 128 KB。它在 `Desktop::render` 里是栈上对象,调用栈不深,无 stack overflow 风险。**溢出策略是 drop**(后续 cmd 丢弃,不 abort)——这守的是 GUI 核心"no exception / never aborts"的铁律:绘制清单爆了最多丢几条指令、画面缺一块,绝不能把整个 host 拖崩。

## Compositor:遍历清单、批量落屏

有了清单,谁来执行?就是 [`core/compositor.hpp`](../../../libs/gui/core/compositor.hpp#L40-L78) 的 `Compositor`。它的 `render` 遍历 `PaintList`、逐条 cmd 调对应的处理函数:

```cpp
void Compositor::render(Surface& staging, const PaintList& list, const PsfFont& font,
                        const ClipRect* outer);
```

这里有两层 clip。`outer` 是 render 的可选入参——一个**脏区矩形**,作为基础裁剪。清单里的 `kClipPush` 指令(由 `Widget::flatten` 推的控件矩形)跟 `outer` 求交,栈顶就是"当前控件矩形 ∩ 脏区矩形"。所以一条 `fill_rect` 最终能不能落屏,要同时过两关:它在脏区里吗(避免画没变化的像素)、它在当前控件的祖先链里吗(避免画出父矩形)。`kClipPop` 弹出栈顶。`kClipPush`/`kClipPop` 不走图元 handler——它们管的是渲染状态(裁剪栈),不是可绘制图元。

每种图元对应一个 handler,构造时注册进 `handlers_[kKindCount]` 数组:

```cpp
Compositor::Compositor() {
    handlers_[static_cast<uint32_t>(CmdKind::kFillRect)]      = h_fill_rect;
    handlers_[static_cast<uint32_t>(CmdKind::kFillRoundRect)] = h_fill_round;
    handlers_[static_cast<uint32_t>(CmdKind::kText)]          = h_text;
    handlers_[static_cast<uint32_t>(CmdKind::kTextGlyph)]     = h_text_glyph;
    handlers_[static_cast<uint32_t>(CmdKind::kTextScaled)]    = h_text_scaled;
}
```

`h_text_glyph` 的实现就是查 PSF 字体拿 glyph 位图、调 `swraster::glyph_blit` 逐位画:

```cpp
void h_text_glyph(Surface& s, const PaintCmd& c, const PsfFont& font, const ClipRect* clip) {
    const uint8_t* bits = font.glyph(static_cast<uint8_t>(c.glyph.ch));
    if (bits != nullptr) {
        glyph_blit(s, c.glyph.x, c.glyph.y, bits, font.width(), font.height(),
                   c.glyph.color, clip);
    }
}
```

这里有个开放封闭的设计:**加图元不炸合成器**。要加一个新形状(圆、线、图片),流程是「加一个 `CmdKind` 枚举 + `PaintCmd` 字段 + swraster 原语 + `set_handler` 注册」,`Compositor::render` 本身一行不改——它只是遍历 + 查表分发。这是用 handler 表替掉 render 里的大 `switch` 的全部意义:每加一个图元不再需要回来动合成器的核心循环。

