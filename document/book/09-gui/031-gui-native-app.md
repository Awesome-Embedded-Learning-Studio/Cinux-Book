---
title: 031 · 原生终端应用:让窗口第一次能"做事"
---

# 031 · 原生终端应用:让窗口第一次能"做事"

> 到 030 为止,我们有了一只会动的桌面:窗口能拖、能关、有 Z 序,鼠标点哪儿它知道。但这些窗口**里面是空的**——内容区一片浅灰,什么都干不了。键盘事件虽然已经能进事件队列,可 `WindowManager::handle_key()` 还是那句 `(void)ev;`,事件进了队列就石沉大海。这一章我们要补上最关键的一刀:让窗口真正能"做事"——具体说,做一个**原生终端窗口**,你点中它、在里头打字,字符就带着光标出现在窗口里。这不是再画一个静态矩形,而是内核里第一次出现一个"跑在窗口里的应用"。

## 这一章我们要点亮什么

一件很直观的事:开机进 GUI 后,屏幕中央出现一个 `Cinux Terminal` 窗口;你用鼠标点中它,它被顶到最前面、拿到焦点;然后你敲键盘,字符一个接一个出现在窗口里,光标跟着移动,写满一行还会自动换行、滚屏。整条链路是:

```text
你敲键盘 → PS/2 键盘发 IRQ1 → handler 把 KeyEvent 包成 GUI 事件塞进队列
        → (沿用 030 的 Mouse::event_queue())
PIT 每个滴答 → gui_tick_callback 排空队列 → 键盘事件交给 WindowManager::handle_key
        → handle_key 把事件转给「当前前台窗口」的 on_key()
        → Terminal::on_key 把字符写进自己的 80×25 字符缓冲
        → render_to_canvas 把缓冲光栅化到窗口离屏画布 → composite() blit 到屏幕
```

这件"打字进窗口"的小事,背后点亮了几个在 030 里还不存在的东西。

这件"打字进窗口"的小事,背后点亮了好几个 030 里还不存在的东西。最底层的一个变化,是 `Window` 第一次变成了多态基类——030 的 `Window` 只是个普通类,窗口管理器存着 `Window*` 数组,却从没打算让窗口"各有各的行为"。可这一章要让"键盘事件送给当前前台窗口、由它自己决定怎么处理",窗口管理器就必须能**在不认识 Terminal 这个具体类型的前提下**把事件派发下去,这就逼着 `Window` 长出虚函数。有了这个入口,键盘事件才第一次被真正消费:030 的 `handle_key` 是个空桩,这一章它变成一行 `focused_->on_key(ev.key)`,事件才真正流进某个窗口。而真正接住这些事件的,是这一章第一个"跑在窗口里的应用"`Terminal`——它继承 `Window`,自带一个 80×25 的字符网格、光标、滚动、一套极简的 ANSI 转义处理,还知道怎么把字符用 PSF 字体画成像素。

还有一条暗线:这个 `Terminal` 的 `on_key` 里,其实已经留了一个"把字符转发给一条管道"的分支。这一章它还是休眠的(没有管道可转发),真正点亮要等下一章我们把管道接上。但现在它已经在那儿了——这也是为什么这一章的终端还只是"自言自语"。

## 为什么现在需要它

回顾 030 给我们留下的家底:`Window` 有标题栏、有离屏 `Canvas`、能命中测试、能被拖动;`WindowManager` 有 Z 序数组、有 `composite()` 全量合成、有从顶向下的命中;事件那边,键盘和鼠标已经共用一条 `Mouse::event_queue()`,PIT 滴答里的 `gui_tick_callback` 会排空它,把鼠标事件喂给 `handle_mouse`、键盘事件喂给 `handle_key`。

唯一的断点就在 `handle_key`:

```cpp
void WindowManager::handle_key(Event& ev) {
    // Reserved for future use (sub-iteration D)
    (void)ev;
}
```

030 的窗口管理器根本不知道"键盘该给谁"。它手里只有一堆 `Window*` 和一个 `focused_` 指针,但 `Window` 上没有任何"我收到了一个按键"的入口。所以这一章要先在 `Window` 上凿出这个入口(虚函数 `on_key`),再让 `handle_key` 把事件顺着 `focused_` 送进去。

至于"窗口里画什么",030 的窗口内容区是 `draw_content()` 抹一层背景色了事。这一章的 `Terminal` 要 override 掉内容绘制,自己往那块离屏画布上光栅化字符。所以这一章补的是**应用层**——在 030 的窗口骨架之上,长出第一个有自己的状态、自己的绘制逻辑、能响应输入的"东西"。

## 设计图

键盘事件从硬件到屏幕的完整旅程,在 031 里长这样:

```text
┌─────────────┐  IRQ1   ┌──────────────────────────┐
│  键盘 PS/2   │────────▶│ Keyboard::irq1_handler    │──┐
└─────────────┘         │  (双路分发:原队列 + GUI)  │  │
                        └──────────────────────────┘  │
                                                      ▼
                                                    enqueue
                                                      │
                                                      ▼
                                                    ┌─────────────────────┐
                                                    │  Mouse::event_queue() │  ← 键盘/鼠标共用一条
                                                    │  (128 环形)           │     (名字带 Mouse 是历史遗留)
                                                    └──────────┬──────────┘
                                                               │ dequeue
                        ┌──────────────────────────┐          │
PIT IRQ0 ──────────────▶│ gui_tick_callback         │──────────┘
                        │  排空队列                  │
                        │  KeyDown/Up → handle_key   │
                        └─────────────┬────────────┘
                                      ▼
                        ┌──────────────────────────┐
                        │ WindowManager::handle_key │
                        │  focused_->on_key(ev.key) │   ← 031 把空桩变成这一行
                        └─────────────┬────────────┘
                                      ▼
                        ┌──────────────────────────┐
                        │ Terminal::on_key          │   ← Window 的子类,多态派发落地
                        │  字符进 screen_[25][80]   │
                        └─────────────┬────────────┘
                                      ▼
                        render_to_canvas() → composite() → flip()
```

这里有个 030 就埋下、但 031 才真正用起来的细节:键盘事件并不走什么 `Keyboard::event_queue()`,而是**复用了 `Mouse::event_queue()`** 这条全局 GUI 队列。`Mouse::event_queue()` 这个名字极具误导性——键盘驱动在 IRQ1 里同样往它塞 `KeyDown`/`KeyUp`。它实质是"全局 GUI 事件队列",挂在 `Mouse` 类上纯粹是历史命名。这个误会在 030 接双路分发时就埋下了,031 只是终于有人(终端)来消费这些键盘事件。

`Terminal` 内部的模型则是一张廉价的字符网格,而不是直接操作像素:

```text
   screen_[ROWS][COLS]   ROWS=25, COLS=80   每个 cell = { char, fg, bg }
   ┌────────────────────────────────────────────┐
   │ 'C' 'i' 'n' 'u' 'x' ' ' ...               │ row 0   ← cursor_y_
   │                                            │
   │ ...                                        │
   │                                            │ row 24
   └────────────────────────────────────────────┘
   ↑ cursor_x_ 指向下一个落字位置

   光栅化(每帧): cell → draw_rect(bg) + 按 PSF glyph 逐位画前景 → 屏幕
   满行: newline() → 触底 scroll_up() 把整屏上移一行,顶行丢弃
```

先维护语义层(字符 + 颜色),只在最后一步光栅化成像素,这样换行、退格、清屏、滚动全都只是在廉价的字符数组上挪数据,代价极低。80×25 这个尺寸也不是随便取的——它正是经典 VGA 文本模式的网格,和 shell 那套输出语义天然对齐。

## 代码路线

### Window 变多态基类:为什么必须虚化

要做"把键盘事件送给当前前台窗口",窗口管理器手里拿的是 `Window*`。如果 `Window` 没有"接收按键"这个入口,管理器就只能 `switch` 判断它到底是不是 `Terminal`——可这么一写,以后每加一种窗口都要回来改 `WindowManager`,耦合死。正确的做法是让 `Window` 提供一个虚函数入口,由子类决定怎么响应。所以 031 给 [window.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/gui/window.hpp) 的 `Window` 加了两个虚函数和一个虚析构:

```cpp
virtual void on_key(KeyEvent& ev) { (void)ev; }
virtual void on_paint(cinux::drivers::Canvas& canvas) { (void)canvas; }
```

连同 `virtual ~Window() = default;` 一起,`Window` 从一个普通类变成了可继承的多态基类。基类给的是空实现(吃掉参数什么都不做),这样老的、不需要响应输入的窗口照样能跑;而 `Terminal` 只要 override 掉 `on_key`,就自动"接管"了自己的键盘输入。

这一步看着只有两行,却是整个应用层的地基。没有虚函数,`WindowManager` 就没法"不认识 Terminal 却能把键盘送进 Terminal"。代价是每个 `Window` 多了一个 vptr——在内核里这点开销可以忽略。

### WindowManager::handle_key 实化 + add_window

入口凿好了,`handle_key` 就从空桩变成真正的一行派发。看 [window_manager.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/gui/window_manager.cpp):

```cpp
void WindowManager::handle_key(Event& ev) {
    // Forward keyboard events to the focused window, if any
    if (focused_ != nullptr) {
        focused_->on_key(ev.key);
    }
}
```

注意它**不做命中测试**——键盘焦点是全局唯一的(就是当前前台窗口),不像鼠标有空间命中。`ev.key` 是 `Event` 联合里那个 `KeyEvent`,按引用透传,子类甚至能改它。键盘只给 `focused_`,鼠标才走 `hit_test`,这正符合桌面 GUI 的惯例:键盘跟着焦点走,鼠标跟着光标走。如果用户点到桌面把焦点清空了,`focused_` 是 `nullptr`,这行就什么都不做,事件被安静丢弃。

光有 `handle_key` 还不够——`Terminal` 是 `Window` 的子类,而 030 的 `WindowManager` 只有 `create(title, w, h)`,它内部自己 `new Window(...)`。管理器没法替我们 `new` 一个 `Terminal`(它不认识这个派生类型)。于是 031 又给 `WindowManager` 加了一个 `add_window(Window* win)`,专门接收外部已经构造好的窗口:

```cpp
uint32_t WindowManager::add_window(Window* win) {
    if (count_ >= MAX_WINDOWS || win == nullptr) {
        return 0;
    }
    windows_[count_] = win;          // 接管所有权
    if (font_ != nullptr) {
        windows_[count_]->draw_title_bar(*font_);
    }
    windows_[count_]->draw_content();
    count_++;
    update_focus();                  // 新窗口置顶,顺带拿到焦点
    return windows_[count_ - 1]->id();
}
```

它和 `create` 的后半段(画标题栏、画内容、置顶、给焦点)是一样的,区别只在 `create` 自己 new,`add_window` 接收你 new 好的、已经定位好尺寸的窗口。所有权随之转移给管理器——`Terminal` 的生命周期归 `WindowManager` 管,外部只持有一个非拥有指针。

### Terminal:80×25 字符缓冲与光标

`Terminal` 本体在 [terminal.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/gui/terminal.hpp) / [terminal.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/gui/terminal.cpp)。它的全部"内存"就是一张字符网格加一个光标:

```cpp
struct TerminalCell {
    char     ch = ' ';
    uint32_t fg = 0x00FFFFFF;   // white
    uint32_t bg = 0x00000000;   // black
};

class Terminal : public Window {
public:
    static constexpr uint32_t COLS = 80;
    static constexpr uint32_t ROWS = 25;
    ...
private:
    TerminalCell screen_[ROWS][COLS];
    uint32_t cursor_x_ = 0;
    uint32_t cursor_y_ = 0;
    uint32_t fg_ = 0x00FFFFFF;
    uint32_t bg_ = 0x00000000;
    bool cursor_visible_ = true;
    cinux::drivers::PSFFont* font_ = nullptr;
    ipc::Pipe* stdin_pipe_  = nullptr;   // 暗线:下一章才点亮
    ipc::Pipe* stdout_pipe_ = nullptr;
};
```

构造函数把窗口尺寸直接从字符网格换算出来,再把网格清成默认的空格:

```cpp
Terminal::Terminal(uint32_t x, uint32_t y, const char* title)
    : Window(title, static_cast<int32_t>(x), static_cast<int32_t>(y),
             COLS * 8,   // 80 chars * 8px per char (approximate)
             ROWS * 16)  // 25 rows * 16px per char (approximate)
{
    for (uint32_t r = 0; r < ROWS; r++)
        for (uint32_t c = 0; c < COLS; c++)
            screen_[r][c] = TerminalCell{};
}
```

窗口的物理像素尺寸 = `COLS*8 × ROWS*16 = 640×400`:每个字符宽 8 像素、高 16 像素,正好对上 PSF 字体的 glyph 尺寸。构造时不接管道——管道由 `init.cpp` 在构造之后用 `set_stdin_pipe` / `set_stdout_pipe` 注入,这样 `Terminal` 的生命周期和管道解耦,不挂管道时它就是一个能独立打字的字符屏。

### on_key:本地回显与通向 shell 的暗线

键盘事件最终落到的就是 `Terminal::on_key`。看它的两条路:

```cpp
void Terminal::on_key(KeyEvent& ev) {
    if (!ev.pressed) return;        // 只处理按下,忽略松开
    if (ev.ascii == 0) return;      // 只处理有 ASCII 的键

    // 挂了 stdin 管道:把字符转发进管道,本地不回显
    // (the shell echoes via stdout —— 回显由 shell 经 stdout 绕回来)
    if (stdin_pipe_ != nullptr) {
        char ch = ev.ascii;
        if (ch == '\r') ch = '\n';  // CR → LF,迁就 shell 的行编辑
        stdin_pipe_->try_write(&ch, 1);
        return;
    }

    // 没挂管道:直接写进本地字符缓冲
    process_char(ev.ascii);
}
```

这一章我们只关心第二条路。`stdin_pipe_` 为空时,按键直接 `process_char` 落到屏幕——这就是"自言自语"模式:你敲什么,窗口里就出现什么,完全本地。

第一条路(`stdin_pipe_` 非空)是这一章留的暗线:挂上管道后,按键不再本地回显,而是 `try_write` 进管道,交给真正在另一头读的 shell,由 shell 把回显内容经 stdout 绕回来。这里有个设计上的硬规矩:**回显永远只走 stdout 这一条路,本地绝不重复画一遍**。为什么这么较真?因为如果本地画一份、shell 又回显一份,你敲一个键就会在屏幕上看到两个字符——这是终端最容易踩的坑。这条规矩下一章才会真正生效,但它的开关就埋在 `on_key` 这两行里。

`on_key` 只接"按下"且"有 ASCII"的键,松开事件和功能键(ascii==0)直接丢。这也意味着回车、换行这类**不走 `on_key`**——它们来自 shell 的输出,走的是下面的 `write()` 路径。这个分工是理解"哪些字符从哪条路进屏"的关键。

### write / process_char / newline / scroll:字符如何落屏

`on_key` 的本地分支和将来 shell 的输出,最终都汇到一个入口 `write(const char* str, uint64_t len)`。它逐字节分发控制字符:

```cpp
void Terminal::write(const char* str, uint64_t len) {
    uint64_t pos = 0;
    while (pos < len) {
        char ch = str[pos];
        if (is_escape(ch)) { handle_ansi(str, len, pos); continue; }
        switch (ch) {
        case '\n': newline();   break;
        case '\r': cursor_x_ = 0;   break;
        case '\b': backspace(); break;
        case '\t': tab();       break;
        default:   process_char(ch); break;
        }
        pos++;
    }
}
```

最底层的"落一个字"在 `process_char`,它只认可打印 ASCII(0x20..0x7E),其余直接丢:

```cpp
void Terminal::process_char(char ch) {
    if (static_cast<uint8_t>(ch) < 0x20 || static_cast<uint8_t>(ch) > 0x7E) return;
    screen_[cursor_y_][cursor_x_] = { /*ch=*/ch, /*fg=*/fg_, /*bg=*/bg_ };
    cursor_x_++;
    if (cursor_x_ >= COLS) { cursor_x_ = 0; newline(); }   // 行末自动回卷
}
```

换行和触底滚动都收口在 `newline()`:

```cpp
void Terminal::newline() {
    cursor_x_ = 0;
    cursor_y_++;
    if (cursor_y_ >= ROWS) { cursor_y_ = ROWS - 1; scroll_up(); }
}
```

注意顺序:先归零 `cursor_x_`,再判 `cursor_y_` 是否越界,**越界时先把光标钳到最后一行 `ROWS-1` 再 `scroll_up()`**。这个顺序不能反——先滚后钳会让光标短暂指向 `screen_[ROWS][...]` 越界。`scroll_up()` 的实现朴素到有点"暴力":把第 1..24 行整体上移一行,顶行(第 0 行)直接被覆盖丢弃,再把最后一行清空:

```cpp
void Terminal::scroll_up() {
    for (uint32_t r = 0; r < ROWS - 1; r++)
        for (uint32_t c = 0; c < COLS; c++)
            screen_[r][c] = screen_[r + 1][c];
    for (uint32_t c = 0; c < COLS; c++)
        screen_[ROWS - 1][c] = TerminalCell{};
}
```

这里要诚实说一句:**没有 scrollback**。滚出屏幕的内容永久丢失,没法往上翻——这是这一章的有意简化。退格 `backspace()` 是行内退一格、行首退到上一行末尾;`tab()` 跳到下一个 8 列制表位。这些控制字符的语义都在 `write()` 的 switch 里集中处理,所以光栅化那一层完全不用管"这是什么字符"。

### 极简 ANSI:只接清屏与归位

`is_escape(ch)` 就是判 `ch == '\033'`(ESC)。一旦命中,`write()` 把后续字节交给 `handle_ansi` 解析 CSI 序列(`ESC [` 开头的那些)。说实话,这个终端的 ANSI 支持简陋得可爱——它只认四个 final byte:

```cpp
switch (ch) {
case 'J': if (param == 2) clear();  return;   // ESC[2J  清屏
case 'H': cursor_x_ = 0; cursor_y_ = 0; return;  // ESC[H  光标归位
case 'K': /* 清光标到行尾 */               return;   // ESC[K  清到行尾
case 'm': /* SGR 颜色 */                   return;   // ESC[m  直接忽略
default:  return;
}
```

而且参数只解析单个数值,`'H'` 无条件归零(所以 `ESC[10;20H` 这种定位光标在这里等价于 `ESC[H`)。`'m'`(SGR,设置颜色)干脆注释一句 `For now, just ignore`。为什么这么克制?因为 shell 实际需要的就是清屏和光标归位这两样——每次重画提示符用得上。颜色、定位、多参数 CSI 都是过度设计,这一章明确不做。解析失败或遇到不认识的序列,`handle_ansi` 直接 `return`,`pos` 已经被推进到序列末尾,不会把转义字节当普通字符画成乱码。

### render_to_canvas:把字符网格光栅化成像素

字符缓冲是语义层,真正画到屏幕靠 `render_to_canvas()`。它遍历每个 cell,先用 cell 的背景色 `draw_rect` 铺一格,再若有字符就按 PSF 字体的 glyph 逐位光栅化:

```cpp
void Terminal::render_to_canvas() {
    if (font_ == nullptr) return;                 // 没字体,啥也不画(后面会讲为什么这是坑)
    auto& cvs = canvas();                         // 用基类 Window 的离屏画布
    uint32_t gw = font_->width(), gh = font_->height();

    for (uint32_t row = 0; row < ROWS; row++) {
        for (uint32_t col = 0; col < COLS; col++) {
            const TerminalCell& cell = screen_[row][col];
            uint32_t px = col * gw;
            uint32_t py = TITLE_BAR_HEIGHT + row * gh;   // 内容画在标题栏下方
            cvs.draw_rect(px, py, gw, gh, cell.bg);
            if (cell.ch > ' ') {
                const uint8_t* g = font_->glyph(static_cast<uint8_t>(cell.ch));
                if (g != nullptr)
                    for (uint32_t gr = 0; gr < gh; gr++)
                        for (uint32_t gc = 0; gc < gw; gc++)
                            if ((g[gr] >> (7 - gc)) & 1)
                                cvs.draw_pixel(px + gc, py + gr, cell.fg);
            }
        }
    }
    // ... 光标块(见下)
}
```

有两个细节值得说。一是 `py = TITLE_BAR_HEIGHT + row * gh`:`TITLE_BAR_HEIGHT` 是 `Window` 基类定的 20 像素,内容必须画在标题栏**下方**,所以 Y 偏移要加上它。二是 PSF glyph 的光栅化:每行是一个字节,最高位(`bits >> (7-gc)`)是最左像素,逐位判断画前景色——这正是 PC Screen Font 字体的逐行位图编码。

光标用的是反色块,不用额外资源:在光标格先用前景色铺满一整格,再把那一格的字符用背景色重画一遍,自然就是反相、还透出底下的字符:

```cpp
if (cursor_visible_) {
    uint32_t cx = cursor_x_ * gw;
    uint32_t cy = TITLE_BAR_HEIGHT + cursor_y_ * gh;
    const TerminalCell& cc = screen_[cursor_y_][cursor_x_];
    cvs.draw_rect(cx, cy, gw, gh, cc.fg);        // 前景铺满
    if (cc.ch > ' ') { /* 用 cc.bg 重画 glyph */ }
}
```

注意 `on_paint(Canvas&)` 那个 canvas 形参是被忽略的——`render_to_canvas` 画的是基类自己的 `canvas()`,不是传进来的那个。所以 `Terminal` 没有独立离屏画布,它复用 `Window` 基类的那块。

### gui_start / tick:谁点火、谁每帧刷新

最后把它们接到一起。[gui_init.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/gui/gui_init.cpp) 的 `gui_start()` 在 031 改了返回类型——从 030 的 `void` 变成 `Terminal*`,因为它要 `new` 出那个终端窗口并把它交出去(下一章的 `init.cpp` 要拿到这个指针去接管道):

```cpp
Terminal* gui_start() {
    cinux::drivers::Mouse::init();
    if (g_screen != nullptr)
        cinux::drivers::Mouse::set_screen_bounds(g_screen->width(), g_screen->height());

    uint32_t term_w = Terminal::COLS * 8;     // 640
    uint32_t term_h = Terminal::ROWS * 16;    // 400
    uint32_t term_x = 80, term_y = 60;
    if (g_screen != nullptr) { /* 能放下就居中 */ }

    auto* term = new Terminal(term_x, term_y, "Cinux Terminal");
    term->set_font(g_font);
    WindowManager::instance().add_window(term);

    cinux::drivers::PIT::set_tick_callback(gui_tick_callback, nullptr);
    return term;
}
```

`set_font(g_font)` 这一句千万别漏——后面调试现场会讲为什么。窗口交给 `add_window` 后被置顶、拿到焦点,所以一开机键盘就喂给它。`PIT::set_tick_callback` 把 `gui_tick_callback` 挂到每个滴答(100 Hz)。这个回调就是整个 GUI 的事件泵和刷新循环:

```cpp
void gui_tick_callback(void* /*ctx*/) {
    auto& wm = WindowManager::instance();
    auto& eq = cinux::drivers::Mouse::event_queue();
    Event ev;
    while (eq.dequeue(ev)) {
        switch (ev.type_) {
        case EventType::MouseMove: case EventType::MouseDown: case EventType::MouseUp:
            wm.handle_mouse(ev); break;
        case EventType::KeyDown:   case EventType::KeyUp:
            wm.handle_key(ev);    break;
        }
    }
    auto* focused = wm.focused();
    if (focused != nullptr) {
        auto* term = static_cast<Terminal*>(focused);   // 见下方警告
        term->poll_output();                             // 轮询 shell 输出(这一章还休眠)
        term->render_to_canvas();                        // 光栅化字符缓冲
    }
    wm.composite();                                      // 全量合成 + flip
}
```

排空事件队列后,对当前前台窗口做一次"轮询输出 + 光栅化 + 合成"。这里有个必须诚实标注的硬编码假设:那行 `static_cast<Terminal*>(focused)` 没有任何类型检查,它赌的就是"当前系统里唯一的前台窗口一定是 `Terminal`"。现在只有一个终端窗口,它当然成立;可一旦以后加了别的窗口类型、而那种窗口恰好被顶到前台,这个 `static_cast` 就是未定义行为。这是已知的技术债,留到有多窗口类型时再上 RTTI 或类型标签解决。

## 调试现场

这一章没有现成的 notes 踩坑记录(本 tag 的 notes 是空的),但有两个从源码里就能看出来的、真实可复现的坑,值得提前点破。

### 窗口打开一片空白:忘了给 Terminal 注入字体

**症状。** 终端窗口明明 new 出来了、交给 `add_window` 了,可见到的内容区却是一片默认背景色,一个字符都没有,光标也看不见。

**根因。** `render_to_canvas()` 的第一行就是 `if (font_ == nullptr) return;`,而 `on_paint()` 同样是 `if (font_ != nullptr) render_to_canvas();`。字体指针是通过 `set_font()` 从外部注入的——如果忘了调,或者在调之前 `g_font` 还是空,那 `font_` 就是 `nullptr`,整段光栅化直接跳过,窗口内容当然空白。这不是渲染逻辑写错了,是"字体这个依赖没注入"。

**防法。** `gui_start()` 里 `new Terminal` 之后、`add_window` 之前,老老实实 `term->set_font(g_font);`。顺序也别反:先 `set_font` 再交给管理器,管理器的 `add_window` 会立刻 `draw_content()`,那时字体已经在位。理解了这一点,以后看到"窗口开出来是空的",第一反应就该是去查字体指针,而不是怀疑 `draw_text`。

### 敲一个键出现两个字符:回显的两条路

**症状。** 终端能用,可每敲一个键,屏幕上蹦出两个一样的字符。

**根因。** 就是 `on_key` 那两条路。在本地回显模式(`stdin_pipe_` 为空)下,按键走 `process_char` 直接落屏——这是对的。可一旦你既挂上了 stdin 管道、又手滑让本地也画了一份,就会本地画一个 + shell 经 stdout 回显一个 = 两个。这就是为什么源码在挂管道的分支里写得很死:`try_write` 之后**立即 `return`**,本地绝不碰 `process_char`。回显被强制收口到"shell 经 stdout 绕回来"这一条路。

这条规矩这一章还用不上(管道是下一章的事),但它的开关就在 `on_key` 里。提前理解它,下一章接管道时就不会在"为什么我打字出现双字符"上卡半天。

## 验证

和 030 一样,这一章的验证分三层:纯逻辑用 host 单测、机内集成用 QEMU kernel 测试、视觉效果用 `make run` 肉眼看。

**第一层:host 单元测试。** 字符缓冲、光标、滚动、ANSI 这些纯逻辑不碰真硬件,在 host 上用 mock 画布测。`test/unit/test_terminal.cpp` 用一个 `MockTerminal`(因为真内核 `Canvas`/`Framebuffer` 拉不起来)把 `Terminal` 的字符逻辑镜像重画一遍,断言每个 cell 和光标位置:

```bash
ctest --test-dir build -R "terminal|window|canvas" --output-on-failure
```

这一组覆盖:构造初始化空格屏、光标(0,0)、窗口尺寸 80*8×25*16、`write` 放字符/换行/CR 回列/行尾换行/触底滚动、`backspace` 删字/行首回上一行、`tab` 跳 8 列、`clear` 重置、`on_key` 处理可打印字符并忽略松开、`ESC[2J` 清屏/`ESC[H` 归位/`ESC[K` 清到行尾。`window`/`canvas` 那几个则顺带验了 031 给基类加的 `on_key` 虚派发、以及 `blit` 改有符号坐标后的负坐标裁剪。

**第二层:QEMU kernel 测试。** 真内核 `Terminal` 依赖 `Framebuffer`/`PSFFont`/`heap`/`GDT`,必须在 QEMU 里跑。在 `main_test.cpp` 里注册了 `run_terminal_tests` 和增量后的 `run_window_manager_tests`:

```bash
cmake --build build --target run-big-kernel-test
```

`run_terminal_tests` 验证真内核 `Terminal` 的构造、`COLS=80/ROWS=25`、`write` 的各种控制字符、滚动、ANSI;`run_window_manager_tests` 新增了"虚函数 `on_key` 经基类指针派发到子类"这一组——正是为 `Terminal` 重写 `on_key` 铺路的测试。

**第三层:视觉效果。** 想亲眼看一个能打字的终端:

```bash
cmake --build build --target run
```

预期:开机进 GUI,屏幕中央一个 `Cinux Terminal` 窗口;鼠标点中它(被顶到最前、拿到焦点),敲键盘,字符带着反色光标出现在窗口里;写满一行自动换行,写满一屏向上滚动。这一步看到字符真的"听话"地出现在窗口里,就说明从 IRQ1 到 `on_key` 到 `render_to_canvas` 的整条链路通了。

不过你会发现一个尴尬的事:这个终端现在只是把你敲的键原样显示出来——它**不知道你在敲什么命令**,也不会执行任何东西。它是个能打字的屏幕,还不是一个能跑 shell 的终端。这就是下一章要解决的。

## 下一站

031 的终端已经能接收键盘、能渲染字符,但它是在**自言自语**:你敲什么它画什么,没有任何东西在"理解"这些输入。我们真正想要的是一个跑着 Cinux shell 的窗口——敲 `help`,shell 执行命令、把结果回显在窗口里。

可这里隔着一道坎:这个 `Terminal` 跑在内核态(它是 GUI 子系统的一部分),而 shell 跑在 ring 3(用户态,024 就写好了)。两个不同特权级、不同地址空间的"东西",怎么把按键从内核 GUI 传给用户态 shell,再把 shell 的输出传回来?

我们需要一条**字节通道**——一头让终端把按键送进去,另一头让 shell 读出来;反方向再来一条,送 shell 的输出。在 Unix 世界里,这个通道有一个现成的、优雅的名字:**管道(pipe)**。下一章我们就从零搭一根内核管道,把它伪装成文件挂到 shell 的 fd 0/1 上,让这个终端第一次跑起真正的 shell。

## 参考

- ECMA-48 — Control Functions for Coded Character Sets,5th edition(1991 年 6 月)(CSI 序列 `ESC[2J`=ED 擦页 ERASE IN PAGE、`ESC[H`=CUP 光标定位、`ESC[K`=EL 擦行、`ESC[m`=SGR,支撑本章 ANSI 解释的边界):https://ecma-international.org/wp-content/uploads/ECMA-48_5th_edition_june_1991.pdf
- OSDev Wiki — PC Screen Font(PSF 字体逐行位图 glyph 编码、width/height 字段,支撑 `render_to_canvas` 的光栅化):https://wiki.osdev.org/PC_Screen_Font
- OSDev Wiki — Text UI(80×25 文本模式网格的历史来源,支撑 `COLS=80/ROWS=25` 取值):https://wiki.osdev.org/Text_UI
