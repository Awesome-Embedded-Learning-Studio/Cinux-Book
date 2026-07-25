---
title: 02 · Console:在像素上盖一层文本网格
---

# Console:在像素上盖一层文本网格

Console 的全部状态,就是一组行列计数和颜色:

```cpp
class Console {
    Framebuffer* fb_   = nullptr;
    PSFFont*     font_ = nullptr;
    uint32_t     col_  = 0, row_ = 0;   // 光标: 第几列、第几行
    uint32_t     cols_ = 0, rows_ = 0;  // 屏幕能容下多少列、多少行
    uint32_t     fg_   = 0x00FFFFFF;    // 前景(默认白)
    uint32_t     bg_   = 0x00000000;    // 背景(默认黑)
};
```

初始化时,它根据 framebuffer 和字体尺寸,算出这个屏幕能容下多少行多少列:

```cpp
void Console::init(Framebuffer& fb, PSFFont& font, uint32_t fg, uint32_t bg) {
    fb_ = &fb;  font_ = &font;  fg_ = fg;  bg_ = bg;
    col_ = 0;   row_ = 0;
    cols_ = fb.width()  / font.width();    // 1024 / 8 = 128 列
    rows_ = fb.height() / font.height();   // 768  / 16 = 48 行
    clear();
}
```

`cols_ = fb.width() / font.width()`——屏幕宽除以字宽,就是一行能塞多少个字符。这一步把「像素」翻译成了「字符格」,后面所有逻辑都按字符格思考。整除会产生一点右边/底部的留白(1024 不是 8 的整数倍时),但对一个控制台无所谓。

写一个字符,是个清晰的状态机:

```cpp
void Console::putc(char c) {
    if (fb_ == nullptr || font_ == nullptr) return;
    switch (c) {
    case '\n': new_line();              break;   // 换行
    case '\r': col_ = 0;                break;   // 回到行首
    case '\b':                           // 回退
        if (col_ > 0) col_--;
        else if (row_ > 0) { row_--; col_ = cols_ - 1; }
        break;
    default:
        if (col_ >= cols_) new_line();            // 到右边界, 先换行
        font_->render_char(*fb_, (uint8_t)c,
                           col_ * font_->width(),   // 像素 x = 列号 × 字宽
                           row_ * font_->height(),  // 像素 y = 行号 × 字高
                           fg_, bg_);
        col_++;
        break;
    }
}
```

几个要点。第一,`col * font_->width()` 把「第 col 列」换算成「像素 x 坐标」,`row * font_->height()` 同理——这是 Console 唯一需要和像素打交道的地方,把字符格映射回 013 的像素世界。第二,可打印字符那一路,先检查 `col_ >= cols_`,到了右边界就**先换行再画**,这是「自动换行」(auto-wrap),免得写出屏幕。第三,`\b`(退格)处理了「已经在行首」的情况:退到上一行最后一列,而不是傻在那里。

换行和滚动是 Console 唯一稍微绕的地方:

```cpp
void Console::new_line() {
    col_ = 0;
    if (row_ + 1 >= rows_) scroll();   // 已在最后一行 → 滚动
    else row_++;                       // 否则单纯下移一行
}
void Console::scroll() {
    uint32_t line_height = font_->height();
    fb_->scroll_up(line_height, line_height, bg_);   // 委托给 framebuffer
}
```

`new_line` 里的判断 `row_ + 1 >= rows_` 是关键:如果光标已经在最后一行,再换行就**不往下走了**(没地方走),而是把整个画面往上滚一行,光标留在最后一行。滚动本身 Console 不亲自搬像素,而是委托给 013 写好的 `Framebuffer::scroll_up`,传一个「字高」进去——滚动的粒度正好是一个字符行,滚完底部正好腾出一行空白继续写。这就是 013 里 `scroll_up` 当初要做成「字节级搬整块 + 清底部空带」的原因:它本来就是为控制台翻页服务的。

Console 还留了一个给 kprintf 用的静态适配函数,这个下一节马上用到:

```cpp
static void Console::console_sink_adapter(char c, void* ctx) {
    auto* con = static_cast<Console*>(ctx);
    if (con) con->putc(c);
}
```

它就是一个「把 kprintf 的字符喂给某个 Console 实例」的跳板,签名 `void(char, void*)`——正好是 kprintf 想要的 sink 类型。
