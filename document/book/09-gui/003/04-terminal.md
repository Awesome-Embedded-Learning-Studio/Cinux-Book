---
title: 04 · 终端控件:TerminalWidget + ANSI + 脏行
---

# 终端控件:TerminalWidget + ANSI + 脏行

## TerminalWidget:字符网格 + ANSI + 脏行跟踪

回到这一章的主角——[`core/widget/terminal.hpp`](../../../third_party/Cinux-GUI/core/widget/terminal.hpp#L32-L97) 的 `TerminalWidget`。它继承 `Widget`,override 了三个 protected hook:`paint_to_list`(画)、`collect_dirty`(报告脏区)、`clear_dirty`(清脏)。它的全部"内存"就是几张并行数组加一个光标:

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

**`fg_colors_`/`bg_colors_` 存的是 ANSI 调色板**索引(0..255),不是 XRGB8888 像素值。索引到像素的翻译在 `paint_to_list` 才做——查 [`palette_color`](../../../third_party/Cinux-GUI/core/widget/terminal.cpp#L19-L33):0..15 是 [`colors::kAnsiPalette`](../../../third_party/Cinux-GUI/core/colors.hpp#L10) 的标准 16 色(黑红绿黄蓝品青白 + bright),16..231 是 6×6×6 立方(每通道取 `0` 或 `55+40*v`),232..255 是灰阶(`8+(idx-232)*10`)。这覆盖了 xterm-256color 的全套色,`ls --color`、彩色 prompt、vim/less 的高亮都能出来。**全程纯整数,无浮点**——这是 GUI 核心的一条铁律(swraster 用 Q8.8 定点也是同源)。

## write / put_char_:字节如何落屏

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

SGR(`m`)是最复杂的一个,因为它支持多 code 序列(`38;5;N` 256 色、`1;31` 加粗红)。看 [`apply_sgr_`](../../../third_party/Cinux-GUI/core/widget/terminal.cpp#L104-L148) 的实现:它先把 `csi_param_` 按 `;` 切成一个 `codes[16]` 数组,**然后遍历**,这样可以前瞻——`38;5;N` 需要看后面两个 code 才能定颜色,遍历到 `38` 就 `i+=2` 跳过 `5;N`。支持的 code 也是子集:`0`/`39` 同时重置 fg 到默认白(7)和 bg 到默认黑(0)、`49` 单独重置 bg、`30-37`/`90-97` 设 fg、`40-47`/`100-107` 设 bg、`38;5;N`/`48;5;N` 设 256 色。bold(1)、italic、truecolor(38;2;r;g;b)忽略——不是没用,是优先级低,真彩色需要 24-bit per cell,内存翻倍,先不做。

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

## paint_to_list:把字符网格翻译成绘制指令

字符缓冲是语义层,真正产出绘制指令靠 [`paint_to_list`](../../../third_party/Cinux-GUI/core/widget/terminal.cpp#L390-L418):

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

## collect_dirty / clear_dirty:只刷真正变化的行

保留模式的核心红利就是脏区重绘。TerminalWidget override 了 [`collect_dirty`](../../../third_party/Cinux-GUI/core/widget/terminal.cpp#L349-L379),只报告真正变化的矩形:

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

