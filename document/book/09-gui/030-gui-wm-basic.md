---
title: 030 · 窗口管理器:让画布活起来
---

# 030 · 窗口管理器:让画布活起来

> 到 029 为止,内核有了一块「画布」——能在屏幕上画矩形、画字、`flip()` 刷新一帧。但那张画是**死的**:开机画一次,之后就再也不变。你动鼠标没反应,敲键盘没反应,更别提什么窗口。这一章,我们把这块静态画布变成一个会动的桌面:接上第一个指针设备(PS/2 鼠标),搭起一套统一的事件管线,做出能拖动、能关闭、有 Z 序的窗口,再让一个窗口管理器每帧把所有窗口合成到屏幕上。做完这一步,内核第一次有了「图形交互」——你点哪儿,它知道。

## 这一章我们要点亮什么

一件最直观的事:GUI 模式开机后,屏幕上出现三个错落的窗口;你拖动标题栏,窗口跟着鼠标走;点窗口把它顶到最前面;点右上角红叉把它关掉。整个链路是:

```text
你动鼠标 → PS/2 鼠标发 IRQ12 → 我们的 handler 读 3 字节包、解出 dx/dy/按键
        → 入一个统一事件队列 EventQueue
PIT 每个滴答 → 把队列里的事件排空 → 交给窗口管理器
        → 窗口管理器算命中、改窗口位置 → 把所有窗口按 Z 序重新合成到屏幕 → flip()
```

这件「拖窗口」的小事背后,点亮了好几个全新的东西:

- **第一个指针设备**。前面 014 接过键盘(IRQ1),这一章接鼠标(IRQ12)。和键盘一样是 PS/2,但鼠标有个键盘没有的麻烦:它只报告**相对位移**,你得自己累积成绝对坐标。
- **第一套统一事件系统**。鼠标事件、键盘事件被收进同一个 `EventQueue`,由一个排空点统一消费。这是 GUI 内核里反复出现的「生产/消费」分离,和 014 的键盘 ring buffer 是同一思路的升级版。
- **第一个窗口管理器**。Z 序、合成、命中测试、拖拽——一个窗口管理器该有的骨架,这一章全搭起来。
- **第一次被逼着正视 System V ABI 栈对齐**。鼠标初始化时碰了 PS/2 控制器,触发了一个一直潜伏的栈对齐 bug,一开机就 `#GP`。这条调试线是这一章最有含金量的部分。

## 为什么现在需要它

回顾 029 给我们留下的东西:[canvas.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/canvas.hpp) 里的 `Canvas` 类,它有一个离屏的 back buffer、一个指向 framebuffer 的 front buffer,会 `draw_pixel` / `draw_rect` / `draw_text`,会 `blit`(把一块画布拷到另一块上)、会 `flip()`(把 back buffer 拷到屏幕)。`gui_init()` 用它画了 10 个随机色矩形 + 一句 `Cinux GUI`,然后 `flip()` 一下,完事。

问题很明显:

第一,**没有输入**。这块画布画完就不动了,内核对外面发生什么一无所知。要交互,得先有「能报告坐标和按键」的东西。

第二,**没有「窗口」这个抽象**。029 的画面是直接往整块屏幕画。可真正的桌面是「一块块独立的矩形区域叠在一起」,每个区域有自己的内容、能被拖动、能被点中、谁压在谁上面有讲究。没有窗口抽象,就没法谈交互——你连「点到的是哪个东西」都说不清。

所以 030 要补两层:底下是**输入管线**(鼠标驱动 + 事件队列),上面是**窗口模型**(Window + WindowManager)。两层合起来,画布才活起来。

030 还顺手给 `Canvas` 加了一个新构造函数 `init(uint32_t w, uint32_t h)`,造一块**不挂 framebuffer** 的纯离屏画布:

```cpp
void Canvas::init(uint32_t w, uint32_t h) {
    front_buf_ = nullptr;          // 没有 front buffer
    width_ = w; height_ = h;
    pitch_ = w * 4;                // 4 字节/像素,无对齐填充
    back_buf_ = new uint32_t[w * h];
    memfill32(back_buf_, 0, w * h);
}
```

为什么需要它?因为窗口要**双缓冲**:每个窗口先画到自己的离屏画布上(标题栏 + 内容),合成时再整块 blit 到屏幕。离屏画布的 `flip()` 是 no-op(没有 front buffer),完全合理。这个小改动是后面整个窗口模型的基石。

## 设计图

整个 030 的交互管线长这样:

```text
┌─────────────┐  IRQ1   ┌──────────────────────────┐
│  键盘 PS/2   │────────▶│ Keyboard::irq1_handler    │──┐
└─────────────┘         │  (双路分发:原队列 + GUI)  │  │
                        └──────────────────────────┘  │
                                                      ▼
┌─────────────┐  IRQ12  ┌──────────────────────────┐  enqueue
│  鼠标 PS/2   │────────▶│ Mouse::irq12_handler      │──────────┐
└─────────────┘         │  (3 字节包 → MouseEvent)   │          │
                        └──────────────────────────┘          │
                                                                ▼
                                                    ┌─────────────────────┐
                                                    │   EventQueue (128)   │  ← 统一事件队列
                                                    │   head_ / tail_      │
                                                    └──────────┬──────────┘
                                                               │ dequeue
                        ┌──────────────────────────┐          │
PIT IRQ0 ──────────────▶│ gui_tick_callback         │──────────┘
                        │  排空队列 → handle_mouse   │
                        │            → composite()  │
                        └─────────────┬────────────┘
                                      ▼
                        ┌──────────────────────────┐
                        │  WindowManager            │
                        │  windows_[64] (Z 序)      │
                        │  命中 / 拖拽 / raise      │
                        └─────────────┬────────────┘
                                      ▼
                        clear(桌面色) → 按 Z 序 blit 各窗口 → 画光标 → flip()
```

窗口在 Z 序里的叠放,和合成时的绘制顺序,是理解「为什么点的是这个窗口」的关键:

```text
windows_[] 数组:  index 0 (最底)  ............  index count-1 (最顶)
合成顺序:        先画 index 0 ──────────────────▶ 最后画 index count-1
命中测试顺序:    从 index count-1 ──────────────▶ 往下到 index 0  (顶上的先被命中)

                  ┌──────────────┐
   index 2 (顶)   │   Window 3    │  ← 先画,但命中时最先检查
                  ├──────┬───────┘
   index 1        │ Win2 │
                  ├──┬───┘
   index 0 (底)   │W1│   ← 最后检查的命中候选
                  └──┘
```

数组的下标既表示 Z 序(0 最底),也直接决定了「画的时候从底往顶、点的时候从顶往底」这两条相反的扫描方向。这个对称性是窗口管理器最核心的设计。

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

为了把键盘也接进这条统一管线,[keyboard.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/keyboard/keyboard.cpp) 的 `irq1_handler` 做了**双路分发**:事件照旧进键盘自己的队列,GUI 构建下再额外拷一份进 `Mouse::event_queue()`:

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

[window.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/gui/window.hpp) 定义的 `Window`,核心思路是**双缓冲**:每个窗口自己拥有一块离屏 `Canvas`(就是前面那个不挂 framebuffer 的版本),标题栏和内容都先画在这块离屏画布上,合成时再 `blit_to` 到屏幕:

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

[window_manager.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/gui/window_manager.cpp) 是这一章的重头戏。先看它怎么存窗口:

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

GUI 的初始化分两个时机,封装在 [gui_init.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/gui/gui_init.cpp),刻意让 `kernel_main` 和 `kernel_init_thread` 都不直接碰 GUI 细节:

- `gui_init(Canvas&, PSFFont&)`:在 [main.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/main.cpp) 的早期(console 之后、开中断之前)调用。它初始化窗口管理器、把 029 的那个 demo 画出来(暗色背景 + 随机矩形 + `Cinux GUI`),存好 screen/font 指针。
- `gui_start()`:在 [init.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/init.cpp) 的 `kernel_init_thread` 里、挂载完 ext2 之后调用。它打印里程碑、初始化鼠标、设置屏幕边界、建三个测试窗口,最后**把 `gui_tick_callback` 注册到 PIT**。

为什么要分两步?因为鼠标初始化会去碰 PS/2 控制器(发 `0xA8` 等命令),而键盘当时已经在用同一个控制器了——这件事必须在开中断之后、且和键盘的初始化顺序协调好才安全。`gui_start` 放在 init 线程里,正好避开 `kernel_main` 那段密集的早期硬件初始化。

注册到 PIT 后,每个滴答(100 Hz)都会跑 `gui_tick_callback`:排空事件队列、把鼠标事件喂给 `handle_mouse`、键盘事件喂给 `handle_key`,然后 `composite()` 一帧。于是桌面就动起来了——只要鼠标有输入,下一帧画面就跟着变。

> 这里有个细节:`handle_key()` 在这一章是空的(`(void)ev;`),键盘事件能进队列,但还没人消费。这是故意的留白——键盘事件真正派上用场,要等下一章。现在先说明它「会被送到前台窗口」,实现留给后面。

## 调试现场

这一章的两个调试故事,一个硬核到值得单独成节,一个则是个「不是 bug 但必须懂」的硬件特性。它们都来自 `document/notes/030/` 下真实记录的踩坑。

### #GP:一个潜伏很久的栈对齐 bug,被鼠标初始化引爆

**现象。** `make run` 启动,刚打印出 `[GUI] ===== Milestone 030: GUI Window Manager =====`,立刻炸一个 `#GP`(General Protection,vector 13):

```text
==== EXCEPTION: #GP (vector 13) ====
  RIP   = 0xFFFFFFFF81001DBB   CS  = 0x0010
  RFLAGS= 0x0000000000010002
  RSP   = 0xFFFF800008047EF8   ...
```

崩溃点不在鼠标代码里,而在**键盘**的 IRQ1 handler。这就很奇怪了——我们这一章动的是鼠标,键盘 014 就写好了、一直好好的,怎么现在炸?

**触发链。** 顺着调用关系捋:`gui_start()` → `Mouse::init()` 去操作 8042 PS/2 控制器(发 `0xA8 / 0x20 / 0x60 / 0xD4 + 0xF4`)。这些控制器命令会让 8042 的状态翻转,**顺带产生一个虚假的 IRQ1**(键盘中断)——这是 PS/2 控制器的已知副作用。于是 CPU 跳进 `irq1_stub` → `Keyboard::irq1_handler()`。而在这一章,这个 handler 里多了一段 GUI 双路分发代码(往 `Mouse::event_queue()` 里 enqueue)。编译器为了优化这段,动用了 XMM 寄存器,生成了一条 `movaps %xmm0, (%rsp)`。`movaps` 要求操作数地址 **16 字节对齐**,而此刻 `(%rsp)` 没对齐,于是 `#GP`。

**根因:x86_64 的 System V ABI 栈对齐规则。** 这条规则要求:进入一个函数的瞬间,`RSP ≡ 8 (mod 16)`——也就是 `(RSP + 8)` 是 16 的倍数(见 System V AMD64 ABI §3.2.2「The Stack Frame」)。编译器就靠这个约定,才敢放心地生成 `movaps` 这种要求 16 字节对齐的 SSE 指令。

我们的 ISR stub 原来没满足它。看 [interrupts.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/interrupts.S) 修复**前**的栈账(以无错误码的 IRQ 为例):

```text
CPU 自动压入(IRQ 无错误码): SS, RSP, RFLAGS, CS, RIP = 5 × 8 = 40 字节
ISR stub 压入:               假错误码 + rax..r15 = 16 × 8 = 128 字节
                                                          合计 168 字节
call handler 压入返回地址:                                   8 字节
                                                          合计 176 字节
```

176 是 16 的倍数,意味着 `call` 之后进入 handler 的瞬间 **RSP ≡ 0 (mod 16)**——和 ABI 要求的 `RSP ≡ 8` 差了 8 字节。handler 内部再 `push %rbx; sub $0x20, %rsp` 调整栈帧后,落到那条 `movaps` 时地址正好没对齐,`#GP` 触发。

**修复:补 8 字节对齐 padding。** 在压完 GPR 后、`call` 之前,额外 `push $0` 垫 8 字节:

```asm
    pushq %r15
    pushq $0                 # 对齐 padding(8 字节)
    leaq 8(%rsp), %rdi       # InterruptFrame* 跳过 padding,仍指向 r15
    call \handler
    addq $8, %rsp            # 弹掉 padding
    popq %r15                # 正常恢复 GPR
```

加了这 8 字节后:40 + 128 + 8(padding) = 176,`call` +8 = 184,`184 ≡ 8 (mod 16)` ✓。handler 入口栈对齐正确,`movaps` 不再炸。

这里有两处必须小心。一是 `leaq 8(%rsp), %rdi`:padding 是临时垫的,传给 C handler 的 `InterruptFrame*` 必须跳过它、仍指向原来的 `r15` 字段,这样 `InterruptFrame` 结构体布局完全不用改。二是恢复时先 `addq $8` 弹掉 padding,再按原顺序 `pop` GPR——顺序不能错。

`ISR_NOERRCODE` 和 `ISR_ERRCODE` 两个宏都改了(异常带错误码的那个,CPU 多压一个错误码,账算下来同样需要这 8 字节 padding 才能满足 ABI)。

**为什么之前从来没炸?** 因为这个 bug 一直在那,只是以前的 IRQ handler 都没让编译器生成 `movaps`。直到这一章给键盘 handler 塞了双路分发、触发了 SSE 优化,才把这个潜伏的对齐问题顶出水面。这也是栈对齐 bug 最阴险的地方:它**静默**——简单 handler 不触发,只有编译器恰好用了对齐敏感的指令才暴露,排查难度高。教训很直接:**ISR stub 必须保证 handler 入口 `RSP ≡ 8 (mod 16)`,这是 ABI 的硬性要求,不是可选项**。

> 顺带一提:修完 #GP 后,链接器还会因为另一个符号报错——`__dso_handle` 未定义。这是因为 `WindowManager::instance()` 里那个 `static WindowManager wm;` 单例**带析构函数**,编译器要把它通过 `__cxa_atexit(func, arg, __dso_handle)` 注册成程序退出时调用的析构。我们的 freestanding 内核没有动态链接,得自己提供这个符号。在 [crt_stub.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/crt_stub.cpp) 里补一个 `void* __dso_handle = nullptr;` 就够了(内核没有 DSO,空指针足矣)。一个对齐 bug 引出一个链接符号,这是「从零搭 GUI」这类大改动典型的连带效应。

### 双光标偏移:这不是 bug,是 PS/2 的宿命

**现象。** 在 QEMU VNC 里,屏幕上**同时出现两个光标**:QEMU 自带的圆点,和我们画的那只箭头。两者之间有个**固定偏移**,而且方向随鼠标初始位置变化——初始化到屏幕中央 `(512, 384)`,我们的箭头在圆点的右下方;初始化到 `(0, 0)`,箭头跑到圆点的左上方;偏移量约等于初始坐标。

**根因。** PS/2 协议**只报告相对位移**(dx/dy),不报告绝对位置(见 OSDev Mouse Input)。VNC 客户端那边的宿主光标是用**绝对坐标**渲染的;我们 guest 这边只能从初始位置开始、不断累加 dx/dy 推算光标位置:

```text
宿主: cursor = (absolute_x, absolute_y)           ← VNC 客户端直接知道
我们: cursor = (init_x + Σdx, init_y + Σdy)        ← 只能累积位移
```

两边从不同的起点出发、累积相同的位移,所以**偏移恒等于初始位置的差值**。PS/2 协议(1980 年代设计)根本没有「获取绝对位置」的命令,这不是我们代码的 bug。

**怎么缓解。** 030 的办法是两手:一是 QEMU 配置加 `-usb -device usb-tablet`(见 [qemu.cmake](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/cmake/qemu.cmake)),让 VNC 的宿主光标改用绝对定位渲染,顺手解决鼠标抓取卡住的问题;二是 guest 侧把鼠标初始位置设成 `(0, 0)`,用户从左上角手动移入对齐。两个光标仍然各自独立,但至少可用。

彻底的解法是写一个真正的 USB HID 驱动,读 USB tablet 的绝对坐标(0~width, 0~height)——但那已经超出这一章的范围了,留作长期目标。这条记录的价值在于:遇到「两个光标对不上」时,先别怀疑自己的绘图代码,去查输入协议本身能不能给绝对坐标。

## 验证

和前面几章一样,030 的验证分三层:纯逻辑用 host 单测、机内集成用 QEMU kernel 测试、视觉效果用 `make run` 肉眼看。

**第一层:host 单元测试。** 鼠标包解析、事件队列、窗口命中、窗口管理器的 Z 序/合成/拖拽,这些纯逻辑不碰真硬件,在 host 上 `-O2` 编、用 `CINUX_HOST_TEST` 门控跑。和 014 一样是「镜像」测法——把内核里的逻辑抄一份到测试里(因为带 `io_inb` 内联汇编、PIC 调用的内核代码在 host 上跑不起来),测「给我这个输入,算出来的事件/状态对不对」:

```bash
ctest --test-dir build -R "mouse|event_queue|window|canvas" --output-on-failure
```

覆盖:`test_mouse`(3 字节包解析、9 位符号扩展、Y 轴翻转、边沿检测、clamp)、`test_event_queue`(环形满/空/回卷/丢弃)、`test_window`(构造、ID 自增、标题栏、内容、关闭按钮命中、`contains`、blit)、`test_window_manager`(create/destroy/raise、Z 序、composite、handle_mouse 拖拽)、`test_canvas`(029 已有 + 新的离屏 `init(w,h)`)。一次全跑也行:

```bash
cmake --build build --target test_host
```

**第二层:QEMU kernel 测试。** 真正跑内核代码、走真 IRQ 的机内测,在 `main_test.cpp` 里注册了四个 GUI 测试套:

```bash
cmake --build build --target run-big-kernel-test
```

它会跑 `run_mouse_event_tests`(鼠标事件流:PS/2 包 → EventQueue → MouseEvent)、`run_window_tests`、`run_window_manager_tests`(create/destroy/raise/拖拽的端到端)、`run_gui_integration_tests`(`gui_init` 接线、键盘双路分发、PIT 滴答回调、鼠标事件经 EventQueue 流到窗口管理器)。这是把前面「镜像测」验证过的逻辑,放到真实的内核 + QEMU + PS/2 模拟器里再验一遍整条管线。

**第三层:视觉效果。** 想亲眼看到三个窗口、亲手拖一下:

```bash
cmake --build build --target run
```

`qemu.cmake` 已经带了 `-usb -device usb-tablet`,所以鼠标抓取和对齐都正常。预期:开机进 GUI 后,屏幕上三个错落的窗口(`Window 1/2/3`),鼠标是一只带黑边的白色箭头;按住左键拖标题栏,窗口跟着走;点窗口把它顶到最前;点右上角红叉关闭。

```text
[GUI] Initialising GUI subsystem...
[GUI] Demo rendered to framebuffer.
...
[GUI] ===== Milestone 030: GUI Window Manager =====
[MOUSE] Mouse enabled (ACK received).
[MOUSE] PS/2 mouse driver initialised.
[GUI] WindowManager initialised with 3 test windows.
[GUI] GUI tick callback registered on PIT.
```

看到 `WindowManager initialised with 3 test windows` 和 `GUI tick callback registered on PIT`,就说明整条输入管线 + 合成循环都起来了。

## 下一站

到 030,我们有了能拖动的窗口,但这些窗口**里面是空的**——内容区就一片浅灰,什么都干不了。键盘事件虽然已经能进事件队列,但 `handle_key()` 还是空的,没人消费。

下一步要解决的自然是:**让窗口里真的能跑东西**。具体说,我们希望键盘事件不再是「进了队列就石沉大海」,而是真正送到当前前台窗口、被它消费——比如在一个窗口里打字,字就出现在那个窗口里。这会把「窗口」从一个会动的矩形,变成一个真正能承载内容的容器。怎么实现,是下一章的事。

## 参考

- OSDev Wiki — Mouse Input(PS/2 鼠标 3 字节包格式、相对位移特性):https://wiki.osdev.org/Mouse_Input
- OSDev Wiki — "8042" PS/2 Controller(初始化命令 `0xA8 / 0x20 / 0x60 / 0xD4 + 0xF4`):https://wiki.osdev.org/%228042%22_PS/2_Controller
- System V Application Binary Interface — AMD64 Architecture Processor Supplement,§3.2.2 The Stack Frame(函数入口 `(%rsp + 8)` 必须是 16 的倍数):https://gitlab.com/x86-psABIs/x86-64-ABI
- QEMU 鼠标光标偏移(VNC 绝对光标 vs PS/2 相对位移):https://torgeir.dev/2024/02/qemu-mouse-cursor-offset/
