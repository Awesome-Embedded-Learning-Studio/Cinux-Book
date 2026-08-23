---
title: 02 · 窗口管理器实现:鼠标/事件/Window/WM
---

# 窗口管理器实现:鼠标/事件/Window/WM

## 代码路线

### 鼠标驱动:PS/2 的 3 字节包,和那个会咬人的 Y 轴

鼠标和键盘共享同一个 8042 PS/2 控制器,但走 AUX(辅助)口、用 IRQ12。初始化序列在 [mouse.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/mouse.cpp) 的 `Mouse::init()`,和键盘那套命令是亲戚:

```cpp
// 1. 启用辅助设备(鼠标)口
io_outb(COMMAND, 0xA8);
// 2. 读配置字节,置 bit1 开 IRQ12,写回
io_outb(COMMAND, 0x20);  config = io_inb(DATA);
config |= 0x02;
io_outb(COMMAND, 0x60);  io_outb(DATA, config);
// 3. 往 AUX 口发 0xF4,激活鼠标的流模式,等一个 ACK(0xFA)
io_outb(COMMAND, 0xD4);  io_outb(DATA, 0xF4);
```

这套 `0xA8 / 0x20 / 0x60 / 0xD4 + 0xF4` 是 8042 控制器的标准命令序列(详见 OSDev 的 "8042" PS/2 Controller 页)。

鼠标的真正难点在**解析**。PS/2 鼠标每次报告一个 **3 字节包**:

```text
byte0:  bit0=左键 bit1=右键 bit2=中键  bit3=恒1(包同步位)
        bit4=X符号 bit5=Y符号  bit6=X溢出 bit7=Y溢出
byte1:  X 位移(8 位)
byte2:  Y 位移(8 位)
```

注意 byte0 的 bit3 恒为 1。`process_byte()` 用它做**包同步**:收到一个字节时,如果它本该是包首(byte0)却没有 bit3,就丢掉,直到对齐。这样就算丢了几个字节,也能重新锁住包边界:

```cpp
if (packet_idx_ == 0) {
    if ((byte & 0x08) == 0) return;   // 不是合法包首,丢掉等对齐
}
```

位移值是 **9 位有符号数**:byte1/byte2 给低 8 位,byte0 的符号位给第 9 位。符号扩展要手动做:

```cpp
int32_t dx = static_cast<int32_t>(b1);
if (b0 & X_SIGN) dx -= 256;   // 9 位有符号扩展
```

然后是**最容易写反的一处**:PS/2 的 Y 轴方向是「向上为正」(鼠标物理上移 dy>0),但屏幕坐标是「向下为正」。所以累加时 Y 要**减**:

```cpp
mouse_x_ += dx;
mouse_y_ -= dy;          // 物理上移(dy>0)= 屏幕往上(y 减小)
...
me.dy = -dy;             // 事件里的 dy 翻成屏幕空间(正=向下)
```

这个符号反转如果写错,鼠标上下移动会反过来——属于那种「能跑,但诡异地别扭」的 bug。累加完再 clamp 到屏幕边界,就得到绝对坐标。

按键用**边沿检测**生成 MouseDown/MouseUp:拿当前 `buttons` 和上一帧的 `prev_buttons_` 做位运算,新置位的位 = 按下,新清零的位 = 松开:

```cpp
uint8_t pressed  = new_buttons & ~prev_buttons_;   // 这帧新按下的
uint8_t released = prev_buttons_ & ~new_buttons;   // 这帧新松开的
```

这样一次「按下不松」不会每帧都报 MouseDown,只在状态翻转的那一刻报。位移只要非零就报一个 MouseMove。所有事件都 `enqueue` 进全局队列。最后别忘了 `PIC::send_eoi(12)`——否则下一次鼠标中断永远不会再投递,这是 PS/2 中断处理的老规矩。

### 事件队列:为什么要有一个「统一排空点」

鼠标和键盘原本各管各的:键盘有自己的 ring buffer(014 做的),鼠标现在也维护自己的坐标。但窗口管理器不想关心「这个事件是鼠标来的还是键盘来的」,它只想从一个地方拿事件。于是 [event.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/gui/event.hpp) 定义了一套统一事件:

```cpp
enum class EventType : uint8_t {
    MouseMove, MouseDown, MouseUp, KeyDown, KeyUp,
};

struct Event {
    EventType type_;
    union { MouseEvent mouse; KeyEvent key; };   // type_ 决定哪个成员有效
};
```

`EventQueue` 是一个 128 容量的环形缓冲,`head_` / `tail_` 两个游标,满了就**静默丢弃**(输入事件丢一两个无所谓,不能阻塞中断)。设计意图是:**生产端是中断 handler,消费端是 PIT 滴答回调,中间隔着一个队列解耦**。中断只管「把事件塞进去就返回」,怎么处理是滴答里的事——和 014 的「ISR 入队 + 轮询出队」一脉相承。

这里有个值得说清楚的点:头文件注释把 `EventQueue` 称作「single-producer / single-consumer」。严格说它有**两个**生产站点(IRQ1 的键盘、IRQ12 的鼠标),所以并不是教科书意义上的单生产者。它之所以能安全工作,是因为生产(输入 IRQ)和消费(PIT 滴答)都发生在中断上下文里,靠中断处理的串行化来保证队列不被同时踩踏,而不是靠什么无锁原子操作。这是一个「靠上下文串行化换来的简化」,在这个阶段够用——但别把它当成可以随便放宽的硬并发保证。

为了把键盘也接进这条统一管线,[keyboard.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/030_gui_wm_basic/kernel/drivers/keyboard/keyboard.cpp) 的 `irq1_handler` 做了**双路分发**:事件照旧进键盘自己的队列,GUI 构建下再额外拷一份进 `Mouse::event_queue()`:

> **tag-bound 说明**:这段内联在 `irq1_handler` 里的 `#ifdef CINUX_GUI` 块是 030 当时的写法。F-GUI 解耦后(CODING-TASTE §14:驱动不持 GUI 依赖),键盘不再 `#include` 任何 GUI 头、也不再直接往 `Mouse::event_queue()` enqueue;改为通过 `Keyboard::register_key_listener`(keyboard.cpp:288)注册回调,GUI 侧在 `gui_init.cpp` 的 `on_key_event` 监听器里消费——双路分发的实质保留,只是接线点从驱动内挪到了驱动外。本章贴的 `#ifdef` 代码块按 tag 030 当时的原文叙述。

```cpp
#ifdef CINUX_GUI
{
    cinux::gui::Event gui_ev{};
    gui_ev.type_ = ev.pressed ? EventType::KeyDown : EventType::KeyUp;
    gui_ev.key = { ev.ascii, ev.scancode, ev.pressed, ev.shift, ev.ctrl, ev.alt };
    cinux::drivers::Mouse::event_queue().enqueue(gui_ev);
}
#endif
```

事件队列挂在了 `Mouse` 类上(`Mouse::event_queue()`),是个历史命名,实质是全局 GUI 事件队列。这段双路分发看起来人畜无害,但**正是它,触发了这一章最硬核的那个 bug**——我们放到「调试现场」细讲。

### Window:为什么要先画到离屏画布上

[window.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/030_gui_wm_basic/kernel/gui/window.hpp) 定义的 `Window`,核心思路是**双缓冲**:每个窗口自己拥有一块离屏 `Canvas`(就是前面那个不挂 framebuffer 的版本),标题栏和内容都先画在这块离屏画布上,合成时再 `blit_to` 到屏幕:

> **tag-bound 说明**:这一章讲的 `Window`(`x_/y_/w_/h_` + `title_[64]` + `canvas_` + 静态 `TITLE_BAR_HEIGHT=20` / `CLOSE_BUTTON_SIZE=14` + `next_id_`)是 030 当时**内核内**的简版。后续的 visor 解耦把整个 GUI 整体外置到 `libs/gui/`,在那里 `window.hpp` 被重写成基于 `Widget` 的复合控件(字段变成 `kTitleBarHeight` / `kCloseButtonSize=20` / `theme_` / `content_` / `on_close_` / `drag_px_` 等,和这一章的简版不再是一回事)。本章一律按 tag 030 当时的内核源码叙述,链接也指向该 tag 的 blob。

```cpp
class Window {
    static constexpr uint32_t TITLE_BAR_HEIGHT = 20;
    static constexpr uint32_t CLOSE_BUTTON_SIZE = 14;
    ...
    int32_t  x_, y_;              // 屏幕坐标
    uint32_t w_, h_;              // 内容区宽高(不含标题栏)
    char     title_[64];
    Canvas   canvas_;             // 离屏 back buffer
    bool     visible_, focused_;
};
```

为什么不直接往屏幕画,非要绕一层离屏缓冲?因为这一章的合成策略是**全量重绘**:每一帧,窗口管理器把整个屏幕 clear 成桌面色,然后从最底到最顶把所有窗口 blit 上去。如果窗口内容直接画在屏幕上,clear 这一下就把所有东西抹了,你还得重画;而且画的过程中用户会看到「画了一半」的中间状态(撕裂)。每个窗口维护自己的离屏画布,合成时只是「把成品整块拷过去」,既快又干净,也不怕被 clear。

`Window` 还自带**命中测试**。`contains(mx, my)` 判断点是否落在窗口矩形内(注意高度要加上标题栏:`total_height() = h_ + TITLE_BAR_HEIGHT`);`is_close_button_hit()` 判断点是否落在右上角那个 14×14 的红叉上。窗口 ID 由静态计数器 `next_id_`(从 1 开始)自动分配,这样窗口管理器不用自己维护 ID 方案。

### WindowManager:Z 序、全量合成、从顶向下的命中

[window_manager.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/030_gui_wm_basic/kernel/gui/window_manager.cpp) 是这一章的重头戏。先看它怎么存窗口:

> 同样是 tag-bound:030 当时这个 `WindowManager` 有 `MAX_WINDOWS=64` 的 `windows_[]` 数组、`composite()` / `hit_test(int32_t,int32_t)` / `handle_mouse()` / `draw_cursor()` / `instance()` 单例等成员。visor 解耦后外置到 `libs/gui/core/widget/window_manager.cpp`,新版自己继承 `Widget`,API 改成 `add_window` / `remove_window` / `raise` / `window_at` / `topmost` / `on_pointer` / `paint_to_list`,这些标签下的成员全都不复存在。本章一律按 tag 030 当时的实现叙述。

```cpp
static constexpr uint32_t MAX_WINDOWS = 64;
Window* windows_[MAX_WINDOWS] = {};   // Z 序数组:0 最底,count-1 最顶
uint32_t count_ = 0;
```

两个设计决定值得讲。第一,**为什么存 `Window*` 而不是值**?因为 `Window` 持有动态分配的 `Canvas`(不可拷贝,`Window` 的拷贝构造被 delete)。Z 序重排(raise、destroy)只需要在数组里**挪指针**,不用搬运对象本身——挪指针是几个赋值,搬对象要重建画布。第二,**为什么是固定 64 上限而不是动态容器**?这是内核里的朴素选择:固定数组、没有动态扩容的内存分配风险,够这个阶段的演示用。

合成这一帧的画面,是 `composite()` 的活:

```cpp
void WindowManager::composite() {
    screen_->clear(DESKTOP_COLOR);                 // 1. 抹成桌面色(暗青 0x00224466)
    for (uint32_t i = 0; i < count_; i++)           // 2. 从底到顶 blit 每个可见窗口
        if (windows_[i]->visible()) windows_[i]->blit_to(*screen_);
    draw_cursor(*screen_);                          // 3. 画鼠标光标(浮在最上面)
    screen_->flip();                                // 4. 一次 flip 送到屏幕
}
```

顺序不能乱:先 clear,再从底到顶画窗口(后画的盖在先画的上面,天然形成正确的叠放),最后画光标(光标永远浮在所有窗口之上),flip 一次成帧。这就是前面设计图里那条「画从底到顶、点从顶到底」的对称性的「画」这一半。

命中的那一半在 `hit_test()`,方向**反过来**,从最顶(`count_-1`)往下找,第一个包含该点的窗口就是被点中的:

```cpp
Window* WindowManager::hit_test(int32_t mx, int32_t my) {
    for (uint32_t i = count_; i > 0; i--) {          // 从顶往下
        uint32_t idx = i - 1;
        if (windows_[idx]->visible() && windows_[idx]->contains(mx, my))
            return windows_[idx];
    }
    return nullptr;
}
```

为什么必须从顶往下?因为窗口会重叠。一个点可能同时落在好几个窗口的矩形里,但用户「看到的、想点的」永远是最上面那个。从顶往下找,第一个命中就是对的。

拖拽的逻辑在 `handle_mouse()` 里,关键是**记录抓取偏移**:

```cpp
case EventType::MouseDown:
    if (!ev.mouse.left) break;
    hit = hit_test(ev.mouse.x, ev.mouse.y);
    if (hit == nullptr) { /* 点到桌面:清焦点 */ break; }
    if (hit->is_close_button_hit(...)) { destroy(hit->id()); break; }  // 点红叉:关
    raise(hit->id());                                                   // 顶上来
    if (点在标题栏内) {
        dragging_ = true;
        drag_offset_x_ = ev.mouse.x - hit->x();   // 记下「抓在窗口的哪个位置」
        drag_offset_y_ = ev.mouse.y - hit->y();
    }
    break;
case EventType::MouseMove:
    if (dragging_ && focused_) {
        focused_->set_position(ev.mouse.x - drag_offset_x_,   // 用偏移还原
                               ev.mouse.y - drag_offset_y_);
        composite();
    }
    break;
case EventType::MouseUp:
    dragging_ = false;
    break;
```

为什么要记 `drag_offset_`?因为如果不记,鼠标一按下,窗口的左上角就会**跳到鼠标位置**,窗口猛地窜一下,体验很糟。记下「鼠标点在窗口内部的哪个相对位置」,移动时用 `鼠标坐标 - 偏移` 还原窗口原点,窗口就稳稳地跟着鼠标走,抓哪儿拖哪儿。这是个看似不起眼、但决定了拖拽手感的小细节。

鼠标光标是 `draw_cursor()` 用一个 16×16 的位图画出来的——经典箭头形状,每个 `uint16_t` 是一行,bit15 是最左像素。画的时候先描一圈黑边再填白心,保证在任何背景上都看得见:

```cpp
for (uint32_t row = 0; row < CURSOR_SIZE; row++) {
    uint16_t bits = k_cursor_bitmap[row];
    for (uint32_t col = 0; col < CURSOR_SIZE; col++)
        if (bits & (0x8000 >> col)) {
            // (px-1,py)/(px+1,py)/(px,py-1)/(px,py+1) 画黑边
            // (px,py) 画白心
        }
}
```

光标位置取自 `Mouse::x()` / `Mouse::y()`,直接画在屏幕画布上(不是任何窗口的画布),所以它永远浮在所有窗口之上——和合成顺序里「最后画光标」是一致的。

### gui_init / gui_start:谁在哪儿点火

GUI 的初始化分两个时机,封装在 [gui_init.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/030_gui_wm_basic/kernel/gui/gui_init.cpp),刻意让 `kernel_main` 和 `kernel_init_thread` 都不直接碰 GUI 细节:

- `gui_init(Canvas&, PSFFont&)`:在 [main.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/030_gui_wm_basic/kernel/main.cpp) 的早期(console 之后、开中断之前)调用。它初始化窗口管理器、把 029 的那个 demo 画出来(暗色背景 + 随机矩形 + `Cinux GUI`),存好 screen/font 指针。
- `gui_start()`:在 [init.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/030_gui_wm_basic/kernel/proc/init.cpp) 的 `kernel_init_thread` 里、挂载完 ext2 之后调用。它打印里程碑、初始化鼠标、设置屏幕边界、建三个测试窗口,最后**把 `gui_tick_callback` 注册到 PIT**。

> **tag-bound 说明**:这两段式初始化是 030 当时的接线。F-GUI-USERSPACE(visor 解耦)后,`gui_init(Canvas&, PSFFont&)` 已删,`gui_init.hpp` 只剩 `void gui_start()`(注册 PS/2 鼠标 + 键盘 listener,把事件双写到 `/dev/event0`);`main.cpp` 改调 `cinux::proc::handoff_framebuffer_to_gui(fb, font, console)` 把 framebuffer 让给 userspace GUI host;`gui_tick_callback`(PIT 滴答排空队列)也随之移除,事件改由 `/dev/event0` push、userspace host 自己 poll。本章一律按 tag 030 当时的实现叙述,链接指向该 tag 的 blob。

为什么要分两步?因为鼠标初始化会去碰 PS/2 控制器(发 `0xA8` 等命令),而键盘当时已经在用同一个控制器了——这件事必须在开中断之后、且和键盘的初始化顺序协调好才安全。`gui_start` 放在 init 线程里,正好避开 `kernel_main` 那段密集的早期硬件初始化。

注册到 PIT 后,每个滴答(100 Hz)都会跑 `gui_tick_callback`:排空事件队列、把鼠标事件喂给 `handle_mouse`、键盘事件喂给 `handle_key`,然后 `composite()` 一帧。于是桌面就动起来了——只要鼠标有输入,下一帧画面就跟着变。

> 这里有个细节:`handle_key()` 在这一章是空的(`(void)ev;`),键盘事件能进队列,但还没人消费。这是故意的留白——键盘事件真正派上用场,要等下一章。现在先说明它「会被送到前台窗口」,实现留给后面。

