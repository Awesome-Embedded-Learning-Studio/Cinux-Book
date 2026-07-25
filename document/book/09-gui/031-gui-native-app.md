---
title: 031 · 原生终端应用:Widget 树 + PaintList 保留模式
---

# 031 · 原生终端应用:Widget 树 + PaintList 保留模式

> 到 030 为止,桌面有了一只会动的窗口骨架:窗口能拖、能关、有 Z 序。但这些窗口**里面是空的**——内容区一片浅色,什么都干不了。这一章要补上最关键的一刀:让窗口真正能"做事"。具体说,做一个**原生终端窗口**——你点中它、在里头打字,字符就带着光标出现在窗口里,还能往里灌 shell 输出,出彩色、能滚动。
>
> 可这一章不是把 030 那套画法再画一个矩形。它换了一种**渲染模型**:从「控件自己拿画布、当场画像素」(即时模式),换成「控件把自己想画的指令收集进一张清单,最后由合成器批量落屏」(保留模式)。前者是 029 那块 `Canvas` 的玩法;后者是这一章要立的 **Widget 树 + PaintList**。这一换,脏区重绘、批量合成、跨进程共享画面全都打开了。窗口、窗口管理器、终端,在这套模型下都只是 Widget 树上的节点。

## 这一章咱们要点亮什么

一件很直观的事:开一个 SDL 窗口,里头跑着一个真 shell;你点中窗口,键盘敲下去,字符带着反色光标出现在窗口里;`ls --color` 出来的目录清单是彩色的;写满一行自动换行,写满一屏向上滚。整条链路是:

```text
你敲键盘 → SDL 把按键事件交给 host
        → host 把可打印字节写进 PTY master fd
        → shell 在 PTY slave 那头读、执行、把结果写回 PTY
        → host 每帧非阻塞 read PTY → TerminalWidget::write(bytes)
        → write 逐字节 put_char_:解析 \n/\r/\b/\t + ANSI SGR/光标/清屏 → 改 cells_ + 标脏
        → Desktop::render 调 root->flatten(PaintList) 把整棵树拍成绘制清单
        → Compositor::render 逐条 cmd 画进 staging Surface(只重画脏区)
        → host 把 staging 推上屏
```

这条链路背后,点亮了四样 030 里还不存在的东西。

**第一样是 Widget 基类**。030 的窗口是个普通类,窗口管理器存着一串窗口指针。可这一章要让"窗口里能放不同的内容"——终端是一种内容,以后还会有按钮、文本框、滑块。如果每种内容都得窗口管理器亲自认识,那每加一种控件就要回来改管理器,耦合死。正解是让所有能画、能命中、能收事件的东西共享一个虚接口 [`Widget`](third_party/Cinux-GUI/core/widget.hpp#L41-L136):它持一个矩形、一串子控件、三个虚 hook——`paint_to_list`(画自己)、`hit_test`(点中谁)、`on_pointer`/`on_key`(收事件)。窗口管理器只跟 `Widget*` 打交道,具体是什么控件由虚派发决定。

**第二样是 PaintList 保留模式**。029 的 `Canvas` 是即时模式:你调一次 `draw_rect`,它当下就把像素写进帧缓冲。这一章换成保留模式:控件不直接画像素,而是把"我想画一个 8×16 的红色矩形""我想在 (x,y) 画一个字符 'A'"这类**绘制指令**塞进一张 [`PaintList`](third_party/Cinux-GUI/core/paint_list.hpp#L109-L138);一帧结束时,合成器 [`Compositor::render`](third_party/Cinux-GUI/core/compositor.hpp#L40-L78) 遍历这张清单、逐条落屏。这听像是多此一举,可它带来三个即时模式给不了的东西:**脏区重绘**(只重画变化了的矩形)、**批量合成**(一帧的指令一次性走完,可以裁剪/排序)、**跨进程共享**(这张清单是纯数据,可以序列化丢给另一个进程的合成器——这是 087 把 GUI host 搬到用户态的地基)。

**第三样是 TerminalWidget**。这是第一个"跑在窗口里的应用"。它继承 `Widget`,自带一张 `cols × rows` 的字符网格、光标、ANSI 转义状态机、一套 256 色调色板。它的 `paint_to_list` 不画像素,而是往清单里塞 `fill_rect`(铺底色 / 非 default 的 cell 背景)+ `text_glyph`(每个非空 cell 的字符)。键盘不进 `on_key`——host 直接把字节写进 PTY,shell 的回显经 PTY 绕回来再 `write` 进控件。这条"输入不经控件、输出才进控件"的分工,是终端和普通输入控件最本质的区别。

**第四样是 Window 和 WindowManager 也都是 Widget**。Window 是**复合控件**:它自己画标题栏 + 圆角 body,还持一个 `content_` 子 widget(终端就挂这儿);WindowManager 是桌面根,自己管一串 `windows_[]`(不用 Widget 框架的 `children_`,原因后面讲)。整棵树就是 `WindowManager → Window → TerminalWidget`,一帧一次 `flatten` 把它拍平成一张 PaintList。

还有一条贯穿全章的暗线:`TerminalWidget` 把"输入字节 → 改 cells_"和"重画"完全解耦。`write()` 改完 cells_ 只标脏(`dirty_rows_[row]=true`),不主动画;真正画是 `Desktop::render` 在帧边界统一做。这就是保留模式的精髓——**状态变更和绘制分离**,控件只负责"我现在长什么样",合成器负责"什么时候、画哪块、怎么画"。

## 为什么现在需要它

回顾 030 给咱们留下的家底:窗口有标题栏、能命中测试、能被拖动;窗口管理器有 Z 序、有从顶向下的命中。事件那边,鼠标和键盘已经能进事件队列。可这些家底是建立在 029 那块 `Canvas` 上的——每个窗口自己持一块离屏画布,`draw_rect`/`draw_text` 当场写像素,合成器把各窗口的画布 blit 到屏幕。这套在"窗口里只画静态背景色"时没问题,可一旦窗口里要画"会变化的内容"(终端、按钮、文本框),三个硬伤就露出来了:

- **没有脏区**。终端敲一个字符,严格说只需要重画那一格;可即时模式下你得把整块离屏画布重画一遍再 blit,90% 的像素是白画的。
- **没有跨进程边界**。`Canvas` 直接写帧缓冲物理地址,这是内核态才能干的活。以后想把 GUI host 搬到用户态(087 那条 `/dev/fb0` mmap 路径),控件层就不能再直接碰像素——它得产出一份"我想画什么"的描述,让另一边的合成器去落屏。
- **没有组合语义**。"窗口里套一个终端、终端里再套一个光标块"这种层级,即时模式下每个东西都得自己算坐标偏移、自己裁剪超出父矩形的像素。一旦层级深了,坐标算错和越界画就是家常便饭。

保留模式三样都解:`PaintList` 是纯数据(`fill_rect` / `text_glyph` / `clip_push` 这些 cmd),脏区可以单独收集(只重画标了脏的控件矩形),跨进程只需要把这张清单搬过边界(087 的 staging buffer 就是干这个的),层级裁剪由 `flatten` 的 clip 栈保证(控件画不出祖先矩形外)。

至于"窗口里画什么",030 的内容区是抹一层背景色了事。这一章的 `TerminalWidget` 要 override 掉内容绘制,自己往清单里塞字符。所以这一章补的是**应用层**——在 030 的窗口骨架之上,长出第一个有自己的状态、自己的绘制逻辑、能响应输入的"东西",并且顺手把整套渲染模型换成保留模式。

## 设计图

一帧的完整旅程,在 031 里长这样:

```text
┌──────────────┐  SDL poll  ┌─────────────────────────────┐
│  键盘 / 鼠标  │───────────▶│ host 主循环(terminal-host) │
└──────────────┘            │  · 鼠标 → wm.process_pointer │
                            │  · 键盘 → pty_write(in_fd)   │
                            │  · read(out_fd) → term.write │
                            └──────────────┬──────────────┘
                                           │ 每帧
                                           ▼
                          ┌──────────────────────────────┐
                          │ Desktop::render(staging, ..) │
                          │  1. root->collect_dirty(Region)│ ← 只收集标了脏的矩形
                          │  2. root->clear_dirty()         │
                          │  3. root->layout()              │
                          │  4. paint_list_.clear()         │
                          │  5. root->flatten(paint_list_)  │ ← 把树拍成 cmd 清单
                          │  6. for 每个 dirty rect:        │
                          │       comp_.render(staging,    │
                          │                     paint_list_,│
                          │                     font, &clip)│ ← 批量落屏(只画脏区内)
                          └──────────────┬──────────────┘
                                         ▼
                          staging Surface → host 推上屏
                          (SDL: per-rect UpdateTexture)
```

这里有两个 029/030 没有的关键动作。一是 `collect_dirty`:控件改了状态只标脏,**不立即画**;帧边界由根统一收集所有标了脏的矩形。二是 `flatten`:整棵 Widget 树递归地把自己想画的指令塞进**同一张** PaintList——每个控件 `clip_push` 自己的矩形(保证画不出去)、调 `paint_to_list` 塞自己的 cmd、递归子控件、`clip_pop`。

`TerminalWidget` 内部的模型则是一张廉价的字符网格,而不是直接操作像素:

```text
   cells_[kMaxCols * kMaxRows]   kMaxCols=120, kMaxRows=50   每个 cell = { char, fg[0..255], bg[0..255] }
   ┌────────────────────────────────────────────┐
   │ '$' 'l' 's' ' ' ...                       │ row 0   ← cur_row_
   │                                          │
   │ ...                                      │
   │                                          │ row rows_-1
   └────────────────────────────────────────────┘
   ↑ cur_col_ 指向下一个落字位置

   paint_to_list(每帧): cell → fill_rect(非 default bg) + text_glyph(ch) → PaintList
   满行: newline_() → 触底 scroll_up_() 把整屏上移一行,顶行丢弃
```

先维护语义层(字符 + 颜色索引),只在 `paint_to_list` 那一步翻译成绘制指令,这样换行、退格、清屏、滚动全都只是在廉价的字符数组上挪数据,代价极低。颜色用的是 ANSI 调色板索引(0..15 标准 16 色、16..231 的 6×6×6 立方、232..255 灰阶),`paint_to_list` 时再查 [`palette_color`](third_party/Cinux-GUI/core/widget/terminal.cpp#L19-L33) 翻成 XRGB8888 像素值。`cols_`/`rows_` 默认 80×25,但 cells_ 的 stride 固定是 `kMaxCols=120`——这样 `set_cols_rows` 把网格变小时不用重布局,只是少用几列。

## 代码路线

### Widget 基类:三个虚 hook + 套娃树

要做"窗口里能放不同内容",所有能画、能命中、能收事件的东西得共享一个虚接口。这就是 [`core/widget.hpp`](third_party/Cinux-GUI/core/widget.hpp#L41-L136) 的 `Widget`。它的核心是三个虚 hook:

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

**`flatten` 是非虚的框架入口,`paint_to_list` 才是子类填的虚 hook。** 这样 clip push/pop 和递归子控件由框架统一管,子类只管"画我自己",不用操心"我的祖先矩形是啥""我的孩子要不要递归"。看 [`core/widget.cpp`](third_party/Cinux-GUI/core/widget.cpp#L33-L43) 的实现就一目了然:

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

**`hit_test` 默认按"子控件后画的在上、先命中"递归。** 看 [`widget.cpp`](third_party/Cinux-GUI/core/widget.cpp#L45-L56):children 从后往前(last-to-first)递归,因为后 add 的 child 画在上面、应该先被点中;都不命中才轮到自己。子类可以 override 成非矩形的命中形状(比如圆角按钮),或者像 Window 那样自定义命中逻辑(标题栏、关闭键、内容区各走各的)。

**`on_pointer`/`on_key` 默认 noop。** 老的、不需要响应输入的控件照样能跑;新控件 override 掉就自动接管了对应事件。注意键盘事件 `on_key` 收的是 [`KeycodePayload`](third_party/Cinux-GUI/core/event_payload.hpp#L41-L45)(ascii + scancode + modifiers 三字节),不是 030 那种 `KeyEvent`——这是因为事件要跨进程传输(087 的 `/dev/event0` 走的就是这套 wire layout),payload 必须 packed、定长。

**`invalidate` 是保留模式的"标脏"入口。** 控件改了状态(写了字、按了按钮、拖了窗口),不主动画,只调 `invalidate(Rect)` 把那块矩形 union 进自己的 `dirty_rect_`。真正的画在帧边界由根统一做。`dirty_self_` 初值是 `true`——控件构造时还没画过,首帧必画;之后 `clear_dirty` 把它清掉,空闲时就不再重画(idle → 0 flush)。

这一步看着是个基类,却是整个应用层的地基。没有这套虚接口,WindowManager 就没法"不认识 TerminalWidget 却能把事件送进去、把它的绘制指令收上来"。代价是每个 Widget 多一个 vptr——这点开销可以忽略,换来的解耦值回票价。

### PaintList:绘制指令的有序清单

控件不直接画像素了,那它产出的"我想画什么"住哪儿?就是 [`core/paint_list.hpp`](third_party/Cinux-GUI/core/paint_list.hpp#L109-L138) 的 `PaintList`——一张定长的绘制指令数组:

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

### Compositor:遍历清单、批量落屏

有了清单,谁来执行?就是 [`core/compositor.hpp`](third_party/Cinux-GUI/core/compositor.hpp#L40-L78) 的 `Compositor`。它的 `render` 遍历 `PaintList`、逐条 cmd 调对应的处理函数:

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

### TerminalWidget:字符网格 + ANSI + 脏行跟踪

回到这一章的主角——[`core/widget/terminal.hpp`](third_party/Cinux-GUI/core/widget/terminal.hpp#L32-L97) 的 `TerminalWidget`。它继承 `Widget`,override 了三个 protected hook:`paint_to_list`(画)、`collect_dirty`(报告脏区)、`clear_dirty`(清脏)。它的全部"内存"就是几张并行数组加一个光标:

```cpp
class TerminalWidget : public Widget {
public:
    static constexpr uint32_t kDefaultCols = 80;
    static constexpr uint32_t kDefaultRows = 25;
    static constexpr uint32_t kMaxCols     = 120;
    static constexpr uint32_t kMaxRows     = 50;
    static constexpr uint32_t kGlyphW      = 8;
    static constexpr uint32_t kGlyphH      = 16;

    void set_cols_rows(uint32_t cols, uint32_t rows);
    void set_theme(const Theme* th) { theme_ = th; }
    void write(const char* data, uint32_t len);
    void write(const char* str);
    void clear();

    uint32_t cursor_col() const { return cur_col_; }
    uint32_t cursor_row() const { return cur_row_; }
    char     cell_at(uint32_t col, uint32_t row) const;
    uint8_t  fg_at(uint32_t col, uint32_t row) const;
    uint8_t  bg_at(uint32_t col, uint32_t row) const;

protected:
    void paint_to_list(PaintList& list) const override;
    void collect_dirty(Region& sink) const override;
    void clear_dirty() override;

private:
    enum class AnsiState : uint8_t { kNormal, kEsc, kCsi, kOsc };

    char     cells_[kMaxCols * kMaxRows]     = {};
    uint8_t  fg_colors_[kMaxCols * kMaxRows] = {};
    uint8_t  bg_colors_[kMaxCols * kMaxRows] = {};
    bool     dirty_rows_[kMaxRows]           = {};
    bool     dirty_all_                      = false;
    uint32_t cols_                           = kDefaultCols;
    uint32_t rows_                           = kDefaultRows;
    uint32_t cur_col_                        = 0;
    uint32_t cur_row_                        = 0;
    uint32_t prev_cursor_row_                = 0;
    uint8_t  cur_fg_                         = 7u;
    uint8_t  cur_bg_                         = 0u;
    const Theme* theme_                      = nullptr;
    AnsiState ansi_state_                     = AnsiState::kNormal;
    char     csi_param_[16]                  = {};
    uint8_t  csi_len_                        = 0u;
};
```

几个设计选择要讲清楚。

**`cells_` / `fg_colors_` / `bg_colors_` 是三张并行数组,不是 `struct{char,fg,bg}`。** 一张 `struct` 看着更整齐,但 `cell_at` 这种"只查字符"的接口会读到无关的 fg/bg 字段,cache 局部性差;并行数组让"扫一遍字符"和"查颜色"分开走,各自的访问模式更紧凑。代价是 `scroll_up_` 要同步挪三张数组——但终端滚动本就是低频操作,这点代价不值一提。

**stride 固定是 `kMaxCols=120`,跟 `cols_` 解耦。** 默认 80 列,可窗口拉宽到 100 列就 `set_cols_rows(100, 25)`——`cols_` 变了,但 cells_ 的内存布局不变(还是 120 一行),只是末尾 20 列不用。这避免了 resize 时重布局的开销,代价是末尾未用的格子占一点内存:每个 cell 位置在三张并行数组里各 1 字节(`char` + `uint8_t fg` + `uint8_t bg` = 3 字节)× 50 行 × 20 列 ≈ 3 KB,无所谓。

**`fg_colors_`/`bg_colors_` 存的是 ANSI 调色板**索引(0..255),不是 XRGB8888 像素值。索引到像素的翻译在 `paint_to_list` 才做——查 [`palette_color`](third_party/Cinux-GUI/core/widget/terminal.cpp#L19-L33):0..15 是 [`colors::kAnsiPalette`](third_party/Cinux-GUI/core/colors.hpp#L10) 的标准 16 色(黑红绿黄蓝品青白 + bright),16..231 是 6×6×6 立方(每通道取 `0` 或 `55+40*v`),232..255 是灰阶(`8+(idx-232)*10`)。这覆盖了 xterm-256color 的全套色,`ls --color`、彩色 prompt、vim/less 的高亮都能出来。**全程纯整数,无浮点**——这是 GUI 核心的一条铁律(swraster 用 Q8.8 定点也是同源)。

### write / put_char_:字节如何落屏

`write(bytes)` 是 TerminalWidget 唯一的数据入口。host 从 PTY 读出 shell 输出,就调它喂进来。它逐字节交给 `put_char_`,后者是一个**有状态的 ANSI 状态机**:

```cpp
void TerminalWidget::put_char_(char ch) {
    switch (ansi_state_) {
    case AnsiState::kEsc:
        if (ch == '[')      { csi_len_ = 0u; ansi_state_ = AnsiState::kCsi; }
        else if (ch == ']') { ansi_state_ = AnsiState::kOsc; }
        else                { ansi_state_ = AnsiState::kNormal; }
        return;
    case AnsiState::kCsi:
        if (ch >= 0x40 && ch <= 0x7E) {         // final byte 结束 CSI
            ansi_state_ = AnsiState::kNormal;
            dispatch_csi_(ch);
        } else if (ch >= 0x20 && ch <= 0x3F && csi_len_ < sizeof(csi_param_)) {
            csi_param_[csi_len_++] = ch;       // 收集 param/intermediate bytes
        }
        return;
    case AnsiState::kOsc:
        if (ch == 0x07) { ansi_state_ = AnsiState::kNormal; }   // BEL 结束 OSC
        return;
    case AnsiState::kNormal:
    default:
        if (ch == 0x1B) { ansi_state_ = AnsiState::kEsc; return; }   // ESC 起头
        break;
    }

    switch (ch) {
    case '\n': newline_();                            return;
    case '\r': cur_col_ = 0;                          return;
    case '\b':  // 0x08 BS: 只移光标,不擦(注释见下文)
        if (cur_col_ > 0) --cur_col_;
        return;
    case 0x7f:  // DEL: busybox 行编辑发 0x7f 当退格(真删字)
        if (cur_col_ > 0) {
            --cur_col_;
            const uint32_t idx    = cur_row_ * kMaxCols + cur_col_;
            cells_[idx]           = 0;
            fg_colors_[idx]       = 0;
            bg_colors_[idx]       = 0;
            dirty_rows_[cur_row_] = true;
        }
        return;
    case '\t': cur_col_ = (cur_col_ / 8u + 1u) * 8u;  return;
    default: break;
    }
    if (static_cast<uint8_t>(ch) < 0x20u) return;     // 其他控制字节丢弃

    const uint32_t idx    = cur_row_ * kMaxCols + cur_col_;
    cells_[idx]           = ch;
    fg_colors_[idx]       = cur_fg_;
    bg_colors_[idx]       = cur_bg_;
    dirty_rows_[cur_row_] = true;
    ++cur_col_;
    if (cur_col_ >= cols_) newline_();                // 行末自动回卷
}
```

这里有两个细节特别值得点破,因为它们都是真踩过坑才长出来的。

**BS(`\b` 0x08)只移光标、不擦字符。** 看注释——ANSI 的 BS 语义就只是"光标左移一格",不删字;真正删字是 DEL(0x7f)。可很多终端实现把 BS 当退格擦字用,结果 shell 行编辑的左箭头(发 `\b`)每按一次就吃掉一个字符——光标扫过去字全没了。这里的修正就是把 BS 和 DEL 严格分开:BS 改 `cur_col_`,DEL 改 `cur_col_` **并且**把那一格的 cell 清 0。

**OSC 序列(`ESC ]` ... `BEL`)整段吞掉。** OSC 是操作系统指令,xterm 用来设窗口标题、超链接这些。终端控件不实现这些功能,但也不能把 OSC 里的字节当普通字符画出来(那会满屏乱码)。所以状态机有一个 `kOsc` 态,从 `ESC ]` 进、到 BEL(`0x07`)出,中间的字节全部消费掉不画。CSI 也一样:从 `ESC [` 进、到 final byte(0x40..0x7E)出,中间的 param bytes 收集进 `csi_param_[16]`,结束时交给 `dispatch_csi_` 解析。

`dispatch_csi_` 支持的 final byte 是一个克制过的子集:`m`(SGR 设颜色)、`H`/`f`(光标定位)、`J`(擦屏)、`A`/`B`/`C`/`D`(光标上下左右一格)。为什么不做全?因为 shell 实际需要的就是这几样——`ls --color` 用 SGR、`clear` 用 `ESC[2J` + `ESC[H`、行编辑用光标移动。其他 CSI(擦行 `K`、滚屏 `S`/`T`、多参数定位)都是过度设计,明确不做。解析失败或不认识的序列,`dispatch_csi_` 走 `default: break`,`csi_len_` 已经被推进到序列末尾,不会把转义字节当普通字符画成乱码。

SGR(`m`)是最复杂的一个,因为它支持多 code 序列(`38;5;N` 256 色、`1;31` 加粗红)。看 [`apply_sgr_`](third_party/Cinux-GUI/core/widget/terminal.cpp#L104-L148) 的实现:它先把 `csi_param_` 按 `;` 切成一个 `codes[16]` 数组,**然后遍历**,这样可以前瞻——`38;5;N` 需要看后面两个 code 才能定颜色,遍历到 `38` 就 `i+=2` 跳过 `5;N`。支持的 code 也是子集:`0`/`39` 同时重置 fg 到默认白(7)和 bg 到默认黑(0)、`49` 单独重置 bg、`30-37`/`90-97` 设 fg、`40-47`/`100-107` 设 bg、`38;5;N`/`48;5;N` 设 256 色。bold(1)、italic、truecolor(38;2;r;g;b)忽略——不是没用,是优先级低,真彩色需要 24-bit per cell,内存翻倍,先不做。

换行和触底滚动收口在 `newline_`:

```cpp
void TerminalWidget::newline_() {
    cur_col_ = 0;
    ++cur_row_;
    if (cur_row_ >= rows_) {
        scroll_up_();
        cur_row_ = rows_ - 1;
    }
}
```

注意顺序:先归零 `cur_col_`,再 `++cur_row_`,**越界时先 `scroll_up_()` 把整屏上移一行、再把光标钳到最后一行 `rows_-1`**(代码里的顺序就是 `scroll_up_(); cur_row_ = rows_ - 1;`)。这个"先滚后钳"看起来像光标会短暂指向 `cells_[rows_*kMaxCols + ...]` 越界——其实不会:`scroll_up_` 内部用自己独立的循环变量 `r`(从 1 遍历到 `rows_`),完全不读 `cur_row_`,所以 `cur_row_ == rows_` 时调它一点事没有;钳制语句紧跟在后面把光标拉回 `rows_-1`,越界只存在于这两行之间、不会被任何 cell 访问读到。`scroll_up_` 的实现是朴素的"整体上移一行":把第 1..rows-1 行挪到 0..rows-2,顶行(第 0 行)被覆盖丢弃,末行清空。三张数组(`cells_`/`fg_colors_`/`bg_colors_`)同步挪——只挪 `cells_` 的话,滚后颜色会错位(字上了、色留原行)。这里要诚实说一句:**没有 scrollback**。滚出屏幕的内容永久丢失,没法往上翻——这是这一章的有意简化。

### paint_to_list:把字符网格翻译成绘制指令

字符缓冲是语义层,真正产出绘制指令靠 [`paint_to_list`](third_party/Cinux-GUI/core/widget/terminal.cpp#L390-L418):

```cpp
void TerminalWidget::paint_to_list(PaintList& list) const {
    const uint32_t defbg = (theme_ != nullptr) ? theme_->surface : 0x00000000u;

    list.fill_rect(rect_.x0, rect_.y0, rect_.width(), rect_.height(), defbg);
    for (uint32_t r = 0; r < rows_; ++r) {
        for (uint32_t c = 0; c < cols_; ++c) {
            const uint32_t idx = r * kMaxCols + c;
            const uint8_t  bgi = bg_colors_[idx];
            if (bgi != 0u) {                       // 非 default bg 才 fill(省 fill_rect)
                list.fill_rect(rect_.x0 + static_cast<int32_t>(c * kGlyphW),
                               rect_.y0 + static_cast<int32_t>(r * kGlyphH), kGlyphW, kGlyphH,
                               palette_color(bgi));
            }
            const char ch = cells_[idx];
            if (ch == 0) {
                continue;
            }
            const uint32_t cfg = palette_color(fg_colors_[idx]);
            list.text_glyph(rect_.x0 + static_cast<int32_t>(c * kGlyphW),
                            rect_.y0 + static_cast<int32_t>(r * kGlyphH), cfg, ch);
        }
    }
    if (cur_col_ < cols_ && cur_row_ < rows_) {
        list.fill_rect(rect_.x0 + static_cast<int32_t>(cur_col_ * kGlyphW),
                       rect_.y0 + static_cast<int32_t>(cur_row_ * kGlyphH), kGlyphW, kGlyphH,
                       0x00FFFFFFu);                 // 光标块:白色实心方块
    }
}
```

几个细节。

**先整块铺 default bg,再逐 cell 处理。** 整块 `fill_rect` 用的是 theme 的 `surface` 色(默认黑),保证默认背景的 cell 一次性盖掉;只有 `bgi != 0`(非 default bg)的 cell 才再补一个 `fill_rect`——这省了大量 fill_rect 指令(默认背景的 cell 不画 bg)。代价是 80×25 满载、每 cell 都非 default bg 时,fill_rect 数会到 2000,加上 2000 个 text_glyph 加光标块,正好压在 PaintList 4096 容量的边缘。实际非 default bg 的 cell 很少(ls 高亮就那么几个),所以稳。

**坐标用 `rect_.x0 + c*kGlyphW`、`rect_.y0 + r*kGlyphH`。** 注意这里**没有加标题栏偏移**——因为 `rect_` 已经是 Window 的 `content_rect`(标题栏下方的区域),`Window::layout` 把 content 的 rect 设好了。TerminalWidget 不需要知道"我外面有没有标题栏",它只在自己的 rect 里画。这就是 Widget 树的好处:每个控件只对自己的矩形负责,父控件(Window)负责把它放到对的位置。

**光标块是白色实心方块,不是反色。** 简单、可见、不依赖 cell 内容。它盖在当前 cell 上面,所以光标位置的字符会被遮住——这是有意简化(真终端的光标是反色透出字符,实现要画"前景铺满 + 用 bg 重画 glyph"两步,这里不折腾)。光标 always-on,不闪烁——闪烁要定时器 + 额外状态,这一章不做。

### collect_dirty / clear_dirty:只刷真正变化的行

保留模式的核心红利就是脏区重绘。TerminalWidget override 了 [`collect_dirty`](third_party/Cinux-GUI/core/widget/terminal.cpp#L349-L379),只报告真正变化的矩形:

```cpp
void TerminalWidget::collect_dirty(Region& sink) const {
    if (!dirty_self_) {
        return;
    }
    if (dirty_all_) {
        sink.add(rect_);                  // scroll/clear -> 整块
        return;
    }
    const int32_t y0 = rect_.y0;
    const int32_t x0 = rect_.x0;
    const int32_t x1 = rect_.x1;
    for (uint32_t r = 0u; r < rows_; ++r) {
        if (dirty_rows_[r]) {             // 只收标了脏的行(每行一个 kGlyphH 高的 rect)
            const int32_t ry0 = y0 + static_cast<int32_t>(r * kGlyphH);
            sink.add(Rect{x0, ry0, x1, ry0 + static_cast<int32_t>(kGlyphH)});
        }
    }
    /* 光标足迹:当前行 + 上一帧光标所在行都要重画 */
    const uint32_t cursor_rows[2] = {cur_row_, prev_cursor_row_};
    for (uint32_t i = 0u; i < 2u; ++i) {
        const uint32_t r = cursor_rows[i];
        if (r < rows_) {
            const int32_t ry0 = y0 + static_cast<int32_t>(r * kGlyphH);
            sink.add(Rect{x0, ry0, x1, ry0 + static_cast<int32_t>(kGlyphH)});
        }
    }
}
```

这里有个细节是光标足迹要单独处理。`put_char_` 改 cell 时只标了**新行**脏(`dirty_rows_[cur_row_]=true`),可光标会移动——上一帧光标在 row 5、这一帧光标跑到 row 6,row 5 那块白色方块不重画就会留在屏幕上成一道拖影。所以 `collect_dirty` 显式把当前行 + 上一帧光标行(`prev_cursor_row_`)都加进脏区。`clear_dirty` 里把 `prev_cursor_row_` 更新成这一帧的 `cur_row_`,为下一帧做准备:

```cpp
void TerminalWidget::clear_dirty() {
    dirty_all_ = false;
    for (uint32_t r = 0u; r < kMaxRows; ++r) {
        dirty_rows_[r] = false;
    }
    prev_cursor_row_ = cur_row_;      // 记住这一帧光标行,下一帧 collect 用
    Widget::clear_dirty();
}
```

这就是为什么 shell 输出一行字、只上传那一行 + 光标那两小块矩形(几千像素),而不是整屏(几十万像素)——上传量砍掉 90% 以上,WSLg 这种 streaming texture upload 慢的环境才扛得住。早期的做法是把整 terminal rect 当脏区 ≈ 全屏,省不了;per-row 才是真省。

### Window:复合控件,自画标题栏 + 持一个 content

Window 也是 Widget,但它是个**复合控件**——自己画标题栏 + body,还持一个 content 子控件(终端就挂这儿)。看 [`core/widget/window.hpp`](third_party/Cinux-GUI/core/widget/window.hpp#L43-L123):

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

`hit_test` 是自定义的,因为 Window 有好几个命中区:关闭键、resize grip、标题栏(可拖)、content 区。看 [`window.cpp`](third_party/Cinux-GUI/core/widget/window.cpp#L77-L96)——关闭键和 resize grip 命中返 `this`(Window 自己处理);标题栏命中也返 `this`(开始拖);content 区命中则递归 `content_->hit_test`(让子控件接住);空 content 区回退到 `this`。这就是复合控件的命中语义:父控件可以选择"这事我自己来"或者"让我的 content 处理"。

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

### WindowManager:桌面根,自管 windows_ 数组

WindowManager 也是 Widget,作桌面根。但它**不用 Widget 框架的 `children_`**——它自己管一个 `windows_[]` 数组。原因在 [`window_manager.hpp`](third_party/Cinux-GUI/core/widget/window_manager.hpp#L11-L20) 的头注释里说得很直白:框架的 `flatten` 是 self→children 顺序,无法表达"光标画在所有窗口之上"(`paint_to_list` 在 children 之前跑)。所以 WM 自管数组、在 `paint_to_list` 里手动按正确顺序画:

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

顺序是 bg → 图标 → 窗口(底到顶),保证后加的窗口画在前面、图标可以被窗口遮住。**鼠标指针不在这张清单里**——它由 [`Compositor::render`](third_party/Cinux-GUI/core/compositor.cpp#L116-L186) 在跑完整张 cmd 清单**之后**补画:一张硬编码的 16×16 单色箭头位图(legacy CinuxOS 的指针资源),每个亮像素画成 1px 灰体 + 1px 白描边,坐标从 `set_cursor(cx, cy, has_cursor)` 拿(`Desktop::render` 调 `root->cursor_pos` 问 WM 鼠标在哪)。这样光标永远画在所有窗口之上、不用塞进 paint_to_list;连带一个副作用——光标的 footprint 必须由 WM 自己 union 进脏区(`process_pointer` 里 `invalidate(old cursor footprint) + invalidate(new)`,见上节"光标拖影"那条判据),否则它不在脏区里、`render` 的 outer clip 会把它跳过。

这个"自管数组、不用 children_"的决定带来一个连带义务:**`collect_dirty` 和 `clear_dirty` 都要 override,显式递归 `windows_[]`**。框架默认的 `Widget::collect_dirty` 只递归 `children_`,而 WM 的 `children_` 是空的——不 override 的话,Window 和它的 content TerminalWidget 的脏区永远到不了 root,屏幕就不更新。看 [`window_manager.cpp`](third_party/Cinux-GUI/core/widget/window_manager.cpp#L159-L191):

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

### Desktop:把树驱动起来

最后是把这一切串起来的 [`Desktop`](third_party/Cinux-GUI/core/widget.hpp#L147-L172)。它持根指针、一个 Compositor、一张 PaintList,驱动 `dispatch_pointer` / `dispatch_key` / `render`:

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

### terminal-host:把真 shell 接上

理论讲完了,看实际怎么把 shell 接到这套 Widget 树上。就是 [`host/terminal_host_main.cpp`](third_party/Cinux-GUI/host/terminal_host_main.cpp)——一个 SDL2 主程序,搭一棵 `WindowManager → Window → TerminalWidget`,spawn `/bin/sh` 在 PTY 里跑,主循环把键盘喂进 PTY、把 PTY 输出喂给 TerminalWidget。看组装部分:

```cpp
WindowManager wm;
wm.set_rect(0, 0, kW, kH);
wm.set_theme(&t);

Window winw;
winw.set_title("Terminal");
winw.set_theme(&t);
winw.set_rect(8, 8, kW - 16, kH - 16);

const Rect     cr   = winw.content_rect();
const uint32_t cols = cr.width() / TerminalWidget::kGlyphW;
const uint32_t rows = cr.height() / TerminalWidget::kGlyphH;

TerminalWidget term;
term.set_theme(&t);
term.set_cols_rows(cols, rows);
term.set_rect(cr.x0, cr.y0, cols * TerminalWidget::kGlyphW, rows * TerminalWidget::kGlyphH);
winw.set_content(&term);            // 终端挂进 Window 的 content 槽
winw.layout();                      // 重算 content rect(其实上面已设好,保险)
wm.add_window(&winw);               // Window 进 WM 的 Z 序

Desktop desktop;
desktop.set_root(&wm);              // WM 作桌面根
```

注意几个细节。**cols/rows 是按 content_rect 算的**,不是写死 80×25——窗口拉多大,终端就多少列。`set_rect` 的尺寸是 `cols * kGlyphW`(8)× `rows * kGlyphH`(16),保证终端 rect 正好被整数个 glyph 填满,不会有半个字。`set_content(&term)` 把 term 挂进 Window 的 content 槽 + 加进 Window 的 `children_`(这样 `flatten` 递归到它)。`desktop.set_root(&wm)` 把 WM 设成根,`Desktop::render` 就从这儿开始 flatten。

spawn shell 走 PTY,不是裸 pipe:

```cpp
setenv("TERM", "xterm-256color", 1);          // 让 ls --color / curses 发 SGR
int       in_fd  = -1;
int       out_fd = -1;
char*     argv[] = {const_cast<char*>("sh"), nullptr};
const int pid    = linux_spawn(nullptr, "/bin/sh", argv, &in_fd, &out_fd);
fcntl(out_fd, F_SETFL, O_NONBLOCK);           // 非阻塞 drain
```

`linux_spawn` 的实现在 [`host/posix_spawn.cpp`](third_party/Cinux-GUI/host/posix_spawn.cpp#L16-L40),用的是 `forkpty`——它 fork 出一个子进程、把子的 stdio 挂到一个 PTY 对上、父进程拿到 **master fd**(双向:write 进 shell stdin、read 出 shell stdout)。`*stdin_fd = *stdout_fd = master`——签名跟 pipe 一样(两个 fd),内部其实是 PTY。为什么用 PTY 而不是裸 pipe?因为 PTY 给 shell 一个**控制终端**,行编辑(左箭头、Home、历史)和 curses 程序(vim/less)才能用。裸 pipe 够 ls/echo,但 curses 会烂。`setenv("TERM", "xterm-256color")` 是配套——shell 判断"要不要发彩色"不只看是不是 tty,还看 `$TERM` 是不是色采的;设成 `xterm-256color` 让 `ls --color` 发 256 色 SGR。

主循环把键盘和 PTY 接通:

```cpp
while (running) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_MOUSEMOTION) {
            /* ... 包成 PointerPayload → wm.process_pointer ... */
        } else if (e.type == SDL_KEYDOWN) {
            switch (e.key.keysym.sym) {
            case SDLK_RETURN:     pty_write(in_fd, "\n", 1);            break;
            case SDLK_BACKSPACE:
            case SDLK_DELETE:    { const char d = 0x7f; pty_write(in_fd, &d, 1); } break;
            case SDLK_TAB:        pty_write(in_fd, "\t", 1);            break;
            case SDLK_UP:         pty_write(in_fd, "\x1b[A", 3);        break;
            case SDLK_DOWN:       pty_write(in_fd, "\x1b[B", 3);        break;
            case SDLK_RIGHT:      pty_write(in_fd, "\x1b[C", 3);        break;
            case SDLK_LEFT:       pty_write(in_fd, "\x1b[D", 3);        break;
            case SDLK_HOME:       pty_write(in_fd, "\x1b[H", 3);        break;
            case SDLK_END:        pty_write(in_fd, "\x1b[F", 3);        break;
            default: break;
            }
        } else if (e.type == SDL_TEXTINPUT) {
            pty_write(in_fd, e.text.text, strlen(e.text.text));
        }
    }

    /* drain shell 输出 → terminal,每帧封顶避免一次刷爆 */
    char     rbuf[1024];
    ssize_t  n;
    uint32_t read_total = 0u;
    while (read_total < 8192u && (n = read(out_fd, rbuf, sizeof(rbuf))) > 0) {
        term.write(rbuf, static_cast<uint32_t>(n));
        read_total += static_cast<uint32_t>(n);
    }

    Region dirty;
    desktop.render(staging, font, &dirty);
    if (dirty.count() > 0u) {                       // per-rect upload(只推脏区)
        const uint32_t pitch = kW * 4u;
        for (uint32_t i = 0u; i < dirty.count(); ++i) {
            const Rect&    r = dirty.rects()[i];
            const SDL_Rect sr{r.x0, r.y0, r.x1 - r.x0, r.y1 - r.y0};
            SDL_UpdateTexture(tex, &sr, buf + r.y0 * pitch + r.x0 * 4u, pitch);
        }
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, nullptr, nullptr);
        SDL_RenderPresent(ren);
    }
    SDL_Delay(16);                                  // ~60 fps
}
```

注意键盘走**双路**。`SDL_KEYDOWN` 只发控制键(Return / Backspace / Tab / Delete / 方向键 / Home / End)——SDL 对可打印字符发的是 `SDL_TEXTINPUT`(含 shift 组合、UTF-8 多字节)。两路互补不重复:控制键走 KEYDOWN 翻成对应字节序列(Backspace → `0x7f`、方向键 → `\x1b[A` 这种 ANSI 序列),可打印键走 TEXTINPUT 直接透传字节。这两路都不进 Widget 树——host 直接 `pty_write` 进 PTY master,根本没经过 `Desktop::dispatch_key`。

shell 的输出才进 Widget 树——host 每帧 `read(out_fd)` 抽一段、`term.write` 喂进 TerminalWidget。**输入不经控件、输出才进控件**——这是终端和普通输入控件的本质区别。普通文本框(后面的 TextBox)是你敲键盘、字符直接进控件状态;终端是你敲键盘、字符先去 shell、shell 决定回显什么、回显再进控件。这条分工让 TerminalWidget 没有 `on_key` override——它不需要。

还有一个细节是 read **每帧封顶 8192 字节**。shell 输出大爆发(比如 `ls /usr/lib` 列几千个文件、加载 bashrc)时,一次性 read 几万字节会让这一帧的 `put_char_` 跑几万次、卡住主循环。封顶 8192 让爆发分摊到多帧,画面保持响应(虽然输出慢一点冒出来)。这是 GUI 单线程 + 输入 SPSC 队列模型的一个典型妥协。

最后,host 拿到 dirty Region 后**逐 rect 上传纹理**:`SDL_UpdateTexture(tex, &sr, buf + r.y0*pitch + r.x0*4u, pitch)` 只更新那块矩形对应的纹理区域,而不是整张 texture。这正是保留模式脏区重绘在 host 层的落地——core 报"这几块变了",host 只把这几块推上 GPU。

## 调试现场

这一章没有现成的踩坑笔记(本 tag 的 notes 是空的),但有三个从源码和注释里就能看出来的、真实可复现的坑,提前点破——它们都长着同一张脸:"保留模式下,谁让出来的那块没人标脏"。

### 敲退格键字符被吃掉:BS 和 DEL 混了

**症状。** shell 里按左箭头(或者某些 shell 配置下的 Backspace),光标扫过的字符一个个消失了,光标停在哪儿、那儿就空了。

**根因。** ANSI 的 BS(`\b` 0x08)语义只移光标、不擦字符;真正删字是 DEL(0x7f)。可早期版本的 TerminalWidget 把 BS 当退格擦字用——`put_char_` 见到 `\b` 就 `--cur_col_` **并擦掉那一格的 cell`**。shell 行编辑发左箭头是 `\x1b[D`(光标左移,纯移不删),可有些 shell 把 Backspace 映射成 `\b`——一旦 BS 被实现成"移 + 擦",每收到一个 `\b` 就吃一个字。

**解法**。源码现在的实现严格分开:BS 只 `--cur_col_`,DEL 才 `--cur_col_` + 清 cell。注释里写得很清楚:`ANSI BS only moves the cursor; ash line-edit shifts the cursor with \b, so erasing here wiped every glyph it passed`。源码见 [`terminal.cpp`](third_party/Cinux-GUI/core/widget/terminal.cpp#L286-L304)。这个判据要带走:**控制字符的语义不能靠猜,得查 ANSI/VT100 规范——BS 移、DEL 擦,是两件事**。

### 光标移动留拖影:dirty 没覆盖旧光标行

**症状。** 终端能用,可每次光标换行(比如敲回车),原来光标位置那块白色方块不消失,屏幕上拖出一串光标残影。

**根因。** `put_char_` 写 cell 时只标了**新行**脏(`dirty_rows_[cur_row_]=true`)。光标从 row 5 跑到 row 6,row 5 那块光标方块不属于"被写过的行",`collect_dirty` 不会把它收进脏区——合成器不重画 row 5,白色方块就留在屏幕上。光标移动越频繁(每敲一个字光标都在动),残影越明显。

**解法**。`collect_dirty` 显式把**当前光标行 + 上一帧光标行**都加进脏区:

```cpp
const uint32_t cursor_rows[2] = {cur_row_, prev_cursor_row_};
for (uint32_t i = 0u; i < 2u; ++i) {
    const uint32_t r = cursor_rows[i];
    if (r < rows_) {
        const int32_t ry0 = y0 + static_cast<int32_t>(r * kGlyphH);
        sink.add(Rect{x0, ry0, x1, ry0 + static_cast<int32_t>(kGlyphH)});
    }
}
```

`clear_dirty` 里把 `prev_cursor_row_` 更新成这一帧的 `cur_row_`,下一帧 `collect` 就能找到"光标刚离开的那一行"。源码见 [`terminal.cpp`](third_party/Cinux-GUI/core/widget/terminal.cpp#L366-L379)。这个判据也通用:**任何"位置会移动的可视元素"都要把旧位置 + 新位置都标脏,否则旧位置必然留残影**——光标如此、拖动窗口如此(`move_to_` 标 old + new)、鼠标指针也如此(`process_pointer` 里 `invalidate` old footprint + new footprint)。

### 关窗留残影:remove_window 漏标 stale footprint

**症状。** 点窗口右上角 "x" 关掉,窗口本身消失了,可它原来盖着的那块矩形——本该露出来的桌面背景或下层窗口——画面上还留着关掉那个窗口的像素(标题栏、内容、边角),像一块"幽灵"贴在那儿,直到下次别的东西触发那一块重画才被覆盖掉。

**根因。** `remove_window` 把 Window 从 `windows_[]` 摘出去之后,WM 自己的 `dirty_rect_` 没把这块矩形加进来。`collect_dirty` 只收 `dirty_self_` 标了的矩形——窗口被摘掉时没人标脏,合成器下一帧不知道"这块要重画",屏幕上那块像素就 stale 了。源码注释把这事说得很直白:这是 core 的一个真 bug,host 之前的" workaround"是每帧都全屏 dirty flush(等于把脏区优化废掉);正解是在摘窗口那一刻把它的 rect 标进脏区。

**解法**。摘窗口前**先 capture 它的 footprint**(`const Rect stale = w->rect()`),摘完立刻 `invalidate(stale)`,跟 `add_window` 的 `invalidate()` 镜像:

```cpp
void WindowManager::remove_window(Window* w) {
    const int32_t idx = index_of_(w);
    if (idx < 0) { return; }
    // Capture the footprint BEFORE unlinking: once the window leaves the list
    // its rect is stale, and core must repaint that area (background / windows
    // below) or the closed window's pixels stay on screen.
    const Rect stale = w->rect();
    /* ... 从 windows_[] 摘掉、--count_、清 press_target_ ... */
    invalidate(stale);                       // 露出来的那块要重画
    if (on_remove_cb_ != nullptr) {
        on_remove_cb_(on_remove_ctx_, w);    // host: 拆 per-window 状态(fd 等)
    }
}
```

源码见 [`window_manager.cpp`](third_party/Cinux-GUI/core/widget/window_manager.cpp#L39-L62)。这跟 Window 的 `move_to_` 标 old footprint 是同一类问题:**保留模式下,"一个会消失/会移动的东西让出来的那块"必须有人显式标脏**——即时模式全屏重画自动解决、保留模式必须显式。这判据在本章里已经是第三次出现了(光标拖影、窗口移动、窗口关闭),值得记死。

## 保留模式 vs 即时模式:为什么换

讲了这么多,值得回头问一句:029 的 `Canvas` 即时模式(DrawRect 当场写像素)有什么不好,非得换保留模式(PaintList 收集指令再批量落屏)?三个理由,正好对应这一章点亮的三个能力。

**第一,脏区重绘。** 即时模式画一次就写一次像素,想"只重画变化的部分"得自己记哪些像素变了。保留模式天然有这个:控件改状态只标脏(`invalidate(Rect)`),`collect_dirty` 在帧边界把脏区收集成一组矩形,合成器只在这些矩形内重画。shell 输出一行字,只上传 704×16 那一小块,而不是整屏 720×440——上传量砍 90% 以上,WSLg 这种 streaming upload 慢的环境才能用。

**第二,批量合成 + 裁剪。** 即时模式下每个 `draw_rect` 立刻写像素,没有"全局视角"。保留模式一帧的指令全在 PaintList 里,合成器可以裁剪(每条 cmd 跟 clip 求交)、可以重排(将来按纹理分批)、可以跳过(clip 外的 cmd O(1) 跳)。窗口层级裁剪也是这么来的——`flatten` 的 clip 栈保证控件画不出祖先矩形,即时模式要做到这点得每个 `draw_*` 自己算偏移 + 裁剪。

**第三,跨进程共享。** 这是 087 把 GUI host 搬到用户态的地基。PaintList 是纯数据(`PaintCmd` 是 POD,无指针除了 `text` 借用的字符串),可以序列化进共享内存或 socket、丢给另一个进程的合成器落屏。087 那条 `/dev/fb0` mmap 路径——ring3 进程 mmap 显存、直接画像素——本质上就是 host 进程持有一个 staging Surface、由它自己的合成器执行 PaintList。即时模式下控件直接写物理显存,这是内核态才能干的活;保留模式把"产出指令"和"落屏"分开,前者任意进程能做、后者才需要显存访问权。这就是为什么 087 能让 GUI host 跑在用户态——它跑的就是这一章这套 Widget 树 + PaintList,只是 host 层从 SDL 换成了 `/dev/event0` + `/dev/fb0`。

至于 shell 字节通道:这一章用的是 POSIX `forkpty`(host 进程和 shell 都在 ring3、同一个 Linux 主机)。真要搬到 Cinux 内核里跑,这条通道就得换成内核的 PTY 设备(`/dev/ptmx` + `/dev/pts/N`,066 立的)或者 AF_UNIX socket(083 立的)——字节从 GUI host 进程的 fd 出去、经内核 PTY/socket、到 shell 进程的 fd 0/1 进来。Widget 树和 PaintList 一行不用改,改的只是 host 层那条字节管道。

## 验证

这一章的验证分三层:host 单元测试(纯逻辑)、ASAN 干净(内存)、真 shell 手动冒烟(视觉效果)。

**第一层:host 单元测试。** 字符缓冲、ANSI、脏区这些纯逻辑不碰真硬件,在 host 上用 ctest 测。终端相关的测试套覆盖了:`test_terminal.cpp`(写字符 / `\n` 换行 / `\r` 覆盖 / 滚动 / `\b` 回退 / flatten 含 kFillRect + kTextGlyph)、`test_terminal_ansi.cpp`(SGR fg 31 红 / 32 绿、reset 0/39、bright 91→9、光标 `[1;1H` 覆盖、`[2J` 清屏)、`test_terminal_bg256.cpp`(bg SGR 41/42、256 色 38;5;200、reset 48;5;100→0、cursor block flatten 含 ≥2 个 fill)。跑法:

```bash
cmake -S third_party/Cinux-GUI -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
ctest --test-dir build -R "terminal|window|widget|dirty|cursor" --output-on-failure
```

这一组还顺带验了 Widget 框架本身——`test_widget.cpp` 覆盖 hit-test 嵌套 child / dispatch 交付 / flatten→execute 像素 / clip 收敛(child 超 parent rect 画不出去),`test_window.cpp` 验 Window 的几何/hit/拖拽/close,`test_window_manager.cpp` 验 Z 序/raise/click-to-raise/cursor 跟踪。

**第二层:ASAN 干净。** host 单测在 push 前开 ASAN 自验,确保虚析构链、new[]/delete[]、借用指针(text cmd 的 `const char*`)都没漏:

```bash
cmake -S third_party/Cinux-GUI -B build-asan -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_FLAGS="-fsanitize=address" \
      -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address"
cmake --build build-asan -j$(nproc)
ctest --test-dir build-asan --output-on-failure
```

这一层特别能抓"PaintList 借用指针 dangling"这类问题——`kText` 的 `const char*` 如果调用方传了栈临时,ASAN 会立刻报 use-after-return。

**第三层:真 shell 手动冒烟。** 想亲眼看一个跑着 shell 的终端:

```bash
cmake -S third_party/Cinux-GUI -B build -DCINUX_HOST_TERMINAL=ON
cmake --build build -- terminal-host
./build/terminal-host          # WSL2: 经 WSLg 显示;敲 shell 命令
```

预期:开一个 SDL 窗口,里头是一个跑着 `/bin/sh` 的终端窗口;鼠标点中它(被顶到最前)、键盘敲命令,字符带着白色光标块出现在窗口里;`ls --color` 出彩色目录清单;`clear` 清屏 + 光标归位;写满一行自动换行,写满一屏向上滚动;标题栏可拖、关闭键可点。这一步看到 shell 真的"听话"地执行命令、结果带颜色地回显,就说明从键盘到 PTY 到 `write` 到 `paint_to_list` 到 Compositor 的整条链路通了。

这一步是非 CI 的眼检——ASAN 抓不到的视觉效果(光标拖影、脏区漏刷、颜色错位)只能靠眼睛。但前面两层已经把逻辑对齐过了,这一层主要验"host 层接线对不对"(PTY 双向通、键盘双路对、per-rect upload 对)。

## 这章没做的

把边界划清楚,免得你以为这套终端已经无所不能。

- **没有 scrollback。** 滚出屏幕顶端的内容永久丢弃,没法往上翻。`scroll_up_` 把顶行覆盖、不留历史缓冲——这是有意简化(真 scrollback 要一张 ring buffer + 上滚时按 PgUp 重画,牵动 dirty 策略,先不做)。
- **光标不闪烁、不反色。** TerminalWidget 的光标是 always-on 的白色实心方块,盖在当前 cell 上(那个字符被遮住)。真终端的光标是反色透出字符,实现要画两步(前景铺满 + 用 bg 重画 glyph),还要定时器驱动闪烁——这章不折腾。
- **ANSI 只做子集。** CSI 支持 SGR `m`(fg/bg/256 色)、光标定位 `H`/`f`、擦屏 `J`、光标移动 `A`/`B`/`C`/`D`;擦行 `K`、滚屏 `S`/`T`、多参数定位明确不做。SGR 里 bold(1)、italic、truecolor(`38;2;r;g;b`)忽略——24-bit per cell 会让 `fg_colors_`/`bg_colors_` 从 1 字节涨到 4 字节,内存翻倍,优先级低。
- **OSC 整段吞。** `ESC ]` ... `BEL` 之间的字节全部消费掉不画——不实现设窗口标题、超链接,但也不让这些字节当普通字符画成乱码。
- **键盘不经 Widget 树。** TerminalWidget 没有 `on_key` override,host 直接 `pty_write` 进 PTY master。这不是缺陷,是终端和普通输入控件(后面的 TextBox)的本质区别:普通控件是"键盘 → 控件状态",终端是"键盘 → shell → 回显 → 控件"。
- **read 每帧封顶 8192 字节。** shell 大爆发(`ls /usr/lib`、加载 bashrc)分摊到多帧,牺牲一点输出速度换主循环响应——GUI 单线程模型下不能让一帧的 `put_char_` 跑几万次卡住。
- **本章的 host 是 SDL2 + POSIX forkpty。** 两者都在 ring3、同一个 Linux 主机。真搬到 Cinux 内核里跑,host 层要换:字节通道换成内核 PTY([066](../07-userland/066-pty.md))或 AF_UNIX socket([083](../17-net/083-af-unix-socket.md)),事件/画像素换成 `/dev/event0` + `/dev/fb0`([087](087-device-interfaces.md))。Widget 树和 PaintList 一行不改——这就是 host-neutral 的意义。
- **保留模式不自动恢复遮挡背景。** Window 移走、Window 关闭露出的那块,都要有人显式 `invalidate` 旧 footprint;即时模式全屏重画自动解决,保留模式必须显式。本章调试现场三个坑(光标拖影、窗口移动、窗口关闭)都是这一条的不同表现。

## 下一站

031 的终端已经能跑真 shell、能出彩色、能滚动。可这个 shell 是从开机那一刻就在跑的——host 一启动就 `forkpty` 出一个 `/bin/sh`,挂在唯一的那个终端窗口上。桌面要是只有一个终端窗口,这套没问题。可 033 要把桌面变成"有一排图标、点哪个开哪个"——终端不再是开机默认出现的那个窗口,而是"点 Shell 图标才该出现"的窗口之一。

这就撞上一个时序矛盾:如果开机就造终端,桌面一进来就有一个终端杵那儿,违背"点图标才开"的交互;可如果开机不造终端,shell 又是开机就起的(它得是第一个 ring-3 进程),它一跑就往 stdout 写,谁来接?

答案在 033b——"懒创建":shell 照旧开机起,它的 stdio 照旧挂在 PTY 上;但 PTY 的另一端不立刻绑终端,而是把 fd 先存进 host 状态;用户点 Shell 图标那一刻,host 才 `new` 一个 TerminalWidget + Window、把它们推上桌面、把存好的 fd 绑上去。在终端出生之前,shell 写出的字节先在 PTY 缓冲里排队。这套"懒创建"的代价是要认真对待"生产者(shell)先于消费者(终端)"的那段时间——这正是 033b 要细讲的地方。

至于 GUI host 搬到 Cinux 内核跑、ring3 进程经 `/dev/event0` 读事件、`/dev/fb0` mmap 画像素——那是 087 的事。这一章的 Widget 树 + PaintList 一行没改,只是 host 层从 SDL 换成了那两条设备接口。字节通道从 POSIX `forkpty` 换成内核 PTY(066)或 AF_UNIX socket(083),也是 host 层的事。core 对这些一无所知——它只认识 Widget、PaintList、Surface,这就是 host-neutral 的意义。

## 小结

031 把 030 那个"会动的空窗口骨架"变成"窗口里真能跑 shell"——但比"加一个终端控件"更重要的,是顺手把整套渲染模型从即时模式换成了保留模式。记住下面几条就够:

- **Widget 基类的接口名是 `paint_to_list(PaintList&)`,不是 `on_paint`**;`flatten` 才是非虚框架入口(clip push → paint_to_list → 递归 child → clip pop),`paint_to_list` 是子类填的 protected virtual hook。`hit_test` / `on_pointer` / `on_key` 是另外几个虚 hook。
- **PaintList 是定长 4096 的 cmd 数组**,7 种 CmdKind;溢出 drop 不 abort(守"core never aborts"铁律)。`kTextGlyph` 单字符内联进 cmd,是为了避免字符密集型控件借用栈临时 `char[]` 当指针导致 dangling——这是这种控件最容易踩的坑。
- **保留模式三红利**:脏区重绘(只重画标了脏的矩形)、批量合成 + clip 栈裁剪(控件画不出祖先矩形)、跨进程共享(PaintList 是纯数据,087 把 GUI host 搬用户态的地基)。
- **ANSI 不能靠猜**:BS(`\b`)只移光标、DEL(0x7f)才擦字符——shell 行编辑的左箭头/退格全靠这条分开才不"吃字"。
- **任何"位置会移动 / 会消失"的可视元素,旧位置 + 新位置都得显式标脏**:光标行(`prev_cursor_row_`)、窗口移动(`move_to_` 标 old footprint)、窗口关闭(`remove_window` capture stale rect)。这是保留模式相对即时模式最容易漏的一类点,本章调试现场踩了三次。
- **WindowManager 自管 `windows_[]` 不用 `children_`**(因为 flatten 是 self→children 序,画不了"光标在最上");连带 `collect_dirty`/`clear_dirty` 必须 override 显式递归 `windows_`,否则 Window 的脏区永远到不了 root。
- **flatten 一次、execute per-rect(幂等)**:N 个脏区跑 N 遍整张 list,clip 外的 cmd O(1) 跳过,N 通常 2~3。
- **终端"输入不经控件、输出才进控件"**:host 把键盘字节直接 `pty_write` 进 PTY,不经 `Desktop::dispatch_key`;shell 输出 read PTY → `term.write` 才进控件。TerminalWidget 因此没有 `on_key` override。
- **forkpty 而非裸 pipe**:PTY 给 shell 一个控制终端,行编辑、历史、curses 才能用;`*stdin_fd = *stdout_fd = master` 签名跟 pipe 一样、内部是 PTY。

这一章之后,Widget 树 + PaintList 这套地基就立住了。后面不管是再加控件(按钮/文本框/滑块,Widget 库里其实都已经在了)、把 GUI host 搬进 Cinux 内核(087)、还是换字节通道(066 PTY / 083 AF_UNIX),都是在这套地基上加 host 适配、不再动 core。031 立的不是"一个终端",是"userspace GUI 的控件框架 + 渲染模型"。

## 参考

- ECMA-48 — Control Functions for Coded Character Sets,5th edition(1991 年 6 月)。CSI 序列:`ESC[m` SGR 设色、`ESC[H` CUP 光标定位、`ESC[J` ED 擦屏、`ESC[A/B/C/D` 光标移动。38;5;N / 48;5;N 的 256 色扩展见 xterm 的 `ctlseqs`:https://invisible-island.net/xterm/ctlseqs/ctlseqs.html
- xterm 256 色 palette(0-15 标准 16 色、16-231 的 6×6×6 立方、232-255 灰阶),支撑 [`palette_color`](third_party/Cinux-GUI/core/widget/terminal.cpp#L19-L33) 的颜色翻译算式:https://github.com/termstandard/colors
- Retained-mode vs immediate-mode GUI(保留模式 vs 即时模式的概念框架,支撑本章 PaintList 保留模式 vs 029 Canvas 即时模式的对比):https://en.wikipedia.org/wiki/Graphical_user_interface#Modes
- Linux `forkpty(3)` / PTY(控制终端、行编辑、curses,支撑 [`linux_spawn`](third_party/Cinux-GUI/host/posix_spawn.cpp#L16-L40) 用 PTY 而非裸 pipe 的选择):https://man7.org/linux/man-pages/man3/forkpty.3.html
