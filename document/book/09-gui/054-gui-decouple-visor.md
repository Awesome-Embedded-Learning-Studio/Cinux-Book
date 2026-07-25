---
title: 054 · GUI 解耦:host-neutral core、一张 Host 表,和甩掉 PIT 中断刷新
---

# 054 · GUI 解耦:host-neutral core、一张 Host 表,和甩掉 PIT 中断刷新

> ⚠️ **tag-bound 提示(2026-07)**:本章基于 F13-B tag——内核 `gui_worker` 线程 + Host 表适配架构。wholesale 后(F-GUI-USERSPACE)GUI 完全 userspace 化:`host_cinux.cpp` / `gui_worker_thread` 已删,GUI 逻辑全在 `third_party/Cinux-GUI/`,由 userspace `/cinux_gui_host` 进程驱动。本章引用的 `kernel/gui/*.cpp` 多数已挪或删,读者请按 F13-B tag 读源码;本章保留 tag-bound 叙事(讲解耦从「内核 gui_worker」→「Host 表」的演进,F13-B 是 userspace 化前的中间态)。

> 029 到 033b 在内核里搭起了一整套桌面:双缓冲画布、窗口管理器、位图图标、终端。但那套 GUI 是**焊死在内核里**的——刷新挂在 PIT 的时钟中断回调里,合成完直接 `flip()` 写帧缓冲,想测一个矩形运算都得把整个内核跑起来,更别提哪天想把它搬去用户态了。这一章把它拆开:GUI 的核心几何/事件/光栅化逻辑抽成一个**不 include 任何内核头**的独立库(`third_party/Cinux-GUI/`),内核这边只留一个填「Host 表」的薄适配单元;刷新从 PIT 中断挪到一个普通线程主动去**拉**,顺手甩掉一笔旧债——production 的 APIC 路由其实只送 1 个 PIT tick,屏幕一直是靠启动时预绘那一帧勉强活着。B 档:验证靠 `cinux-gui` 库能脱离内核独立编译跑测试(不需要 QEMU),加上内核测试里新增的 region / dirty / swraster 用例全绿。
>
> 一条诚实的边界先说在前头:本章搭的是**解耦骨架**——把 core 拔出来、把 Host 表立起来、把刷新挪到线程。至于「把源码里残存的 `#ifdef CINUX_GUI` 全归到 CMake 开关、补一组 USB 空壳让 GUI 在真 xHCI 驱动落地前也能链接」这套收尾,时间上排在 xHCI 驱动之后,留到后面;本章讲到那里会停住。

## 这章咱们要点亮什么

1. **被焊死的桌面长什么样**:刷新在 PIT 中断里、合成直写帧缓冲、GUI 代码和内核搅在一起——以及为什么这样既不可靠也不能动。
2. **Host ABI 表**:core 和宿主之间**唯一**的硬接缝,一张函数指针表。谁填表,core 就跑在谁上面。
3. **pump()**:core 侧唯一的主循环,排空输入 → 合成一帧 → 把脏矩形 flush 出去。它不知道自己跑在内核态还是用户态。
4. **甩掉 PIT 中断**:从「中断推(push)」改成「线程拉(pull)」,刷新的可靠性从押在一个已知不稳的硬件特性上,转成押在调度器自己掌控的时间片上。
5. **脏矩形**:只把这一帧真正变了的像素推给显示端,空闲时一帧都不推。
6. **两件收尾的事**:事件枚举为什么从 `EventType` 改名 `EventCode`(扁平命名空间撞名的教训),以及两段几乎一样的「建栈 + 跳用户态」代码怎么收敛成一个共享函数。

## 被焊死的桌面:刷新挂在时钟中断里

先看要修的「病」。在 029–033 那套 GUI 里,屏幕刷新是这么驱动的:向 PIT 注册一个回调,每次 IRQ0 时钟中断就在 **ISR 上下文里**排空鼠标键盘事件、合成一帧、把整帧 `flip()` 到帧缓冲。这是「内核/中断把刷新推给 GUI」的模型。

这个模型有两个病根,而且一个比一个要命。

第一个,**在中断里跑渲染本身就是雷区**。中断上下文里任何意外的阻塞——合成时撞上一个还没就绪的状态、某条路径多绕了一下——都会把整条 IRQ 路径钉死,表现就是卡屏或黑屏。中断是「要尽快交还」的上下文,在里面干一整帧的活本来就违和。

第二个更隐蔽,也才是真正的动机。看 `gui_init.cpp` 里 `gui_start()` 的这段注释:

```cpp
// Composite the desktop once now (icons registered) so the staging back
// buffer is populated. Ongoing refresh is driven by the gui_worker thread
// calling pump() in a loop (see init.cpp), NOT by a PIT IRQ callback.
// This removes the GUI's dependency on PIT tick delivery, which only fires
// once under APIC routing on the production path (pre-existing F4 issue) --
```

`gui_init.cpp:241` 这句点破了一笔藏了很久的债:当内核走 APIC 路由(050 之后的生产配置),PIT 的 IRQ0 **其实只触发一次**。这是 050 把中断从 8259 PIC 切到 APIC 路由时留下的一个已知问题——legacy PIT 那条 IRQ0 在生产配置里被投递一次就不再来了(050 讲过这个路由差异)。也就是说,那个「每次时钟中断刷新一帧」的回调,在生产路径上根本没被反复调用——屏幕一直是靠启动时 `gui_start()` 预绘的那一帧 + 偶尔的输入事件硬撑着。把刷新押在 PIT tick 的到达上,等于押在一个**已知不稳**的机制上。

所以要做的不是给旧路径打补丁,而是换一个模型:**让一个普通可调度线程去主动拉(pull)刷新**,刷新的可靠性从「PIT tick 到不到」转成「这个线程被不被调度」——后者是调度器自己完全掌控的时间片,比硬件特性硬得多。但要做这件事,得先解决一个前置问题:GUI 代码现在和内核搅在一起,放进哪个线程都还是内核态那一坨。于是先得**把 GUI 从内核里拔出来**。

## Host ABI 表:core 和宿主之间唯一的接缝

拔出来的办法,是给 GUI 立一道**硬边界**:边界这边是「host-neutral core」——只懂 GUI 的事(矩形代数、事件泵、光栅化),不认识 framebuffer、不认识 IRQ、不认识进程结构;边界那边是「宿主」——今天是咱们这个内核,明天可能是 SDL 模拟器,后天可能是用户态 server。两边之间**只通过一张表**说话。

这张表就是 `third_party/Cinux-GUI/core/host.hpp` 里的 `Host`:

```cpp
struct HostCore {
    /* 显示后端:把一块脏矩形从 staging 缓冲推到显示屏(core → host) */
    void (*flush)(void* ctx, int x, int y, int w, int h, const void* pixels,
                  uint32_t stride, PixelFormat fmt);
    /* 反向通知:host 告诉 core「上一次异步 flush 完成了」。本内核用同步 flush,
     * 用不到它,填 nullptr;留给将来异步后端(DMA)用。注意方向和 flush 相反。*/
    void (*flush_complete)(void* ctx);
    /* 输入后端:从队列里取一个事件,空了返回 false */
    bool (*poll_event)(void* ctx, EventHeader* out, uint16_t out_cap);
    /* 把取出来的事件应用到宿主自己的 GUI 状态上 */
    void (*dispatch_event)(void* ctx, const EventHeader* ev, const void* payload);
    /* 合成一帧,报告哪些矩形脏了 + staging 缓冲在哪 */
    void (*render_frame)(void* ctx, Frame* frame);
    uint32_t (*now_ms)(void* ctx);   /* 时间 */
    void* (*alloc)(void* ctx, size_t n);  /* 内存 */
    void (*free)(void* ctx, void* p);
    void (*log)(void* ctx, const char* fmt, ...);
};

struct HostDesktop {  /* 桌面扩展:起进程。不是所有宿主都能 spawn,可空 */
    int (*spawn)(void* ctx, const char* path, char* const argv[], int* stdin_fd, int* stdout_fd);
};

struct Host {
    HostCore     core;
    HostDesktop* desktop; /* 不能 spawn 的宿主填 nullptr */
    void*        ctx;     /* 透传给每个回调的不透明上下文 */
};
```

（`host.hpp:46` 起。）这张表里全是**函数指针**——core 永远不直接调 framebuffer 或鼠标队列,它调的是 `host->core.flush(...)` 这种间接调用。这就引出这一整弧最关键的一句话:**谁填这张表,core 就跑在谁上面**。

咱们这个内核在 `kernel/gui/host_cinux.cpp` 里填它。`cinux_host_init()` 就是「填表的现场」:

```cpp
void cinux_host_init(cinux::drivers::Framebuffer* fb) {
    g_fb = fb;
    g_cinux_host.core.poll_event     = cinux_poll_event;     // → Mouse::event_queue()
    g_cinux_host.core.dispatch_event = cinux_dispatch_event;
    g_cinux_host.core.render_frame   = cinux_render_frame;   // → WindowManager::composite()
    g_cinux_host.core.flush          = cinux_flush;          // → 逐行搬进 VBE framebuffer
    g_cinux_host.core.flush_complete = nullptr;              // 同步 flush,用不到反向通知
    g_cinux_host.core.now_ms         = cinux_now_ms;         // → PIT::get_uptime_ms()
    g_cinux_host.core.alloc          = cinux_alloc;
    g_cinux_host.core.free           = cinux_free;
    g_cinux_host.core.log            = cinux_log;
    g_cinux_desktop.spawn = cinux_spawn;                      // → create_shell_terminal()
    g_cinux_host.desktop  = &g_cinux_desktop;
}
```

（`host_cinux.cpp:371`。）每一个函数指针都指向内核里已有的设施:`poll_event` 去鼠标事件队列里取、`render_frame` 让窗口管理器合成、`flush` 把脏矩形逐行搬进 VBE 帧缓冲那块 volatile MMIO、`now_ms` 读 PIT 的 uptime。于是同一份 core,填上这张表就能在内核里跑起来。

> **为什么是函数指针表,不是 C++ 虚函数/抽象基类?** 因为表是纯数据、不带 vtable/RTTI/异常这些「特权包袱」。core 因此能保持 freestanding 友好——只用 `<stdint.h>`/`<stddef.h>`,任何 hosted 编译器都能编。表项允许 `nullptr`,`pump()` 对每个回调先判空再调,所以**半填的表也是安全的**:不能 spawn 的宿主,`desktop` 填 `nullptr` 就行,不会逼它编一个永远没人调的空函数。

这道边界带来的好处,不只是「代码整齐」。它在物理上把 GUI 切成了两份能分别编译的代码:core(`third_party/Cinux-GUI/core/`)平台无关,`kernel/gui/` 只剩一个 host adapter 的翻译单元。`kernel/gui/CMakeLists.txt` 的注释把这件事说得很直白:

> `kernel/gui/` keeps ONLY the Cinux host adapter — the single host-specific TU that fills the ABI table against the kernel's drivers/framebuffer. The core compiles standalone.

**怎么证明 core 真的 host-neutral?** 子模块里带了一个零内核的 `host/fake_host_main.cpp`:它手填一张假表(`fake_poll_event` 永远返回 false、`fake_render_frame` 报一个测试矩形、`fake_flush` 记一下调用次数),然后照常调 `cinux::gui::pump(&h)`。这个程序用普通的 g++/clang++ 就能编、能跑、能断言「NULL-host 不崩 / 空闲跳过 / 脏矩形几何全对」。驱动内核的那同一份 core,驱动一个无内核的假宿主**只靠换一张表的填充**——这就是 host-neutral 的可证伪证据,也是未来 SDL/X11/Wayland adapter 的种子。

## pump():不感知自己在哪条道上的主循环

core 那一侧到底拿这张表干什么?全在 `pump()` 一个函数里。它是 GUI 主循环的**一圈**,函数体里没有任何宿主头文件,只做固定三步:

```cpp
void pump(Host* host) {
    if (host == nullptr) return;
    /* 1. 排空输入:poll_event 取一个、dispatch_event 派一个,直到队列空 */
    if (host->core.poll_event != nullptr && host->core.dispatch_event != nullptr) {
        while (host->core.poll_event(host->ctx, hdr, sizeof(buf)))
            host->core.dispatch_event(host->ctx, hdr, payload);
    }
    if (host->core.render_frame == nullptr || host->core.flush == nullptr) return;
    /* 2. 请宿主合成一帧,拿回「哪些矩形脏了 + staging 缓冲在哪」;没变化就什么都不推 */
    host->core.render_frame(host->ctx, &frame);
    if (frame.count == 0 || frame.pixels == nullptr) return;   /* idle 跳过 */
    /* 3. 把每个脏矩形 flush 出去 */
    for (uint32_t i = 0; i < frame.count; i++) {
        const Rect& r = frame.rects[i];
        host->core.flush(host->ctx, r.x0, r.y0, r.x1 - r.x0, r.y1 - r.y0,
                         frame.pixels, frame.stride, frame.format);
    }
}
```

（`pump.cpp:33`,idle 跳过在 `:62`。)`pump()` 自己**做不了任何 GUI 的事**——它没有窗口管理器、没有渲染器、不知道屏幕在哪。它只是个协议骨架:排空输入、要一帧、把脏区推走。整个函数对宿主是什么类型一无所知,这就是「不感知自己跑在内核态还是用户态」的实现机制。

注意第 2 步里那个 idle 跳过:宿主报告 `frame.count == 0`(这一帧什么都没变),`pump()` 直接 `return`,**连一个像素都不 flush**。这是后面「空闲不空转」的关键。

内核这边驱动它的,就是那个新的 `gui_worker` 线程:

```cpp
void gui_worker_thread() {
    cinux::lib::kprintf("[GUI] Worker thread started\n");
    while (true) {
        cinux::gui::pump(&cinux::gui::cinux_host());
        Scheduler::yield();
    }
}
```

（`init.cpp:32`。）一个裸的 `while (true) { pump(); yield(); }`——这就是 GUI 现在唯一的驱动循环,没有任何别的路径。

## 甩掉 PIT 中断:从「推」到「拉」

有了 `pump()` 和 worker 线程,前面那笔 PIT 的债就能还了。

反转**之前**:刷新是「中断推给 GUI」——PIT 的 IRQ0 回调里 drain 输入 + 合成,押在 tick 到达上(APIC 下其实只到一次)。
反转**之后**:刷新是「线程主动拉」——`gui_worker` 在 `while` 里反复 `pump()`,押在「worker 被调度」上。

`gui_start()` 现在不再注册任何 PIT 回调,而是合成一次桌面、把整屏标脏(让头一圈 `pump()` 把初始桌面推上去)、然后填 Host 表:

```cpp
wm.composite();
wm.invalidate_all();
cinux::lib::kprintf("[GUI] desktop composited; refresh driven by gui_worker pump loop.\n");
cinux_host_init(g_screen != nullptr ? g_screen->framebuffer() : nullptr);
```

（`gui_init.cpp:247`。）注释把动机写得很清楚:从今往后屏幕存活**不再依赖 PIT tick 到不到**,worker 的 pump 循环自己会把它撑住。

一个容易看走眼的地方:PIT 那套回调机制**没有删,只是没人注册了**。`set_tick_callback` / `invoke_tick_callback` / `tick_callback_` 这些符号在 `CINUX_GUI` 下还在(`pit.cpp:128` 起),但全树没有任何地方再调 `set_tick_callback`,所以 `tick_callback_` 是 `nullptr`,`invoke_tick_callback()` 实际是个 no-op。看到这些代码别以为是反转没做完——退役的是「注册」,不是「机制」。

更关键的一点:`pit.cpp:80` 的 `irq0_handler` 里,`invoke_tick_callback()`(GUI,现在是 no-op)和 `Scheduler::tick()`(调度)是**两条独立的语句**:

```cpp
void PIT::irq0_handler(InterruptFrame* /*frame*/) {
    ...
    cinux::arch::irq_eoi(0);
#ifdef CINUX_GUI
    invoke_tick_callback();        // GUI —— 现在是 no-op
#endif
    cinux::proc::Scheduler::tick();// 调度 —— 照常推进
}
```

（`pit.cpp:91` 和 `:94`。）这两条调用独立,是反转能安全做的前提:退役 GUI 回调不会连带把时间片轮转也废掉。如果它们当年是写成一个调用,这步就动不了。

## 脏矩形:只推真正变了的像素

`pump()` 第 2 步的 idle 跳过,靠的是宿主能准确报告「这一帧哪些矩形变了」——也就是**脏矩形**。

先说为什么需要它。屏幕上绝大多数帧只有一小块像素真的变了:鼠标挪一格、终端吐一个字符。如果每帧都把整屏(1024×768,约 3 MB)从 staging 缓冲整块搬到帧缓冲,绝大多数搬运是浪费——尤其帧缓冲走的是 MMIO 总线时。脏矩形的思路:渲染照常全帧画进 back buffer(保证内容恒新),但只把**这一帧变了的那几块**矩形覆盖的像素推给显示端。

Cinux 给这套几何立了一条硬规矩,写在 `region.hpp` 顶部:**宁可多推,绝不漏推**。多推几个没变的像素,是看不见的性能损失;漏推一个变了的像素,是屏幕上残留旧内容的视觉 bug。这条规矩贯穿下面所有的设计。

脏矩形运算建立在**半开区间**的 `Rect` 上:`[x0, x1) × [y0, y1)`,左/上含、右/下不含。一个宽 4 的矩形写成 `{0,0,4,4}`,覆盖的是第 0、1、2、3 列(不是 4 列)。这样两个相邻矩形 `{0,0,4,4}` 和 `{4,0,8,4}` 无缝拼接——共享的那条边 `x=4` 只属于右边那个,不重叠也不漏缝。退化(空)矩形的判据也就干净了:`x0 >= x1 || y0 >= y1`(`region.hpp:47`)。

一帧里「变了」的往往不止一块,所以用一个 `Region`——一个**定容**的矩形集合来累积它们:

```cpp
class Region {
public:
    static constexpr uint32_t kMaxRects = 32;
    void add(const Rect& r);
    ...
private:
    Rect     rects_[kMaxRects];
    uint32_t count_ = 0;
};
```

（`region.hpp:113` 起的 `Region` 类,容量常量 `kMaxRects = 32` 在 `:116`。)定容、不分配堆——这是为了在禁堆的上下文里也能跑。那塞到第 33 个怎么办?它不报错也不扩容,而是把当前所有矩形 + 新矩形的**包围盒**算出来,整个 `Region` 坍缩成这一个矩形:

```cpp
void Region::add(const Rect& r) {
    if (r.empty()) return;
    if (count_ < kMaxRects) { rects_[count_++] = r; return; }
    /* Overflow: collapse the whole region to its bounding box ... */
    Rect bbox = bounds();
    bbox      = rect_union(bbox, r);
    count_    = 0;
    rects_[count_++] = bbox;
}
```

（`region.cpp:51`。）这就是「宁可多推绝不漏推」的落地:包围盒盖住了所有原本的矩形(可能也盖了些本来没变的像素),但绝不会漏掉任何一个变了的像素。对脏区来说,过覆盖是性能问题,欠覆盖才是正确性问题,所以一律取前者。

> **测试里一个真实的坑。** 坍缩不是「一旦溢出就永远只剩一个矩形」:坍缩后 `count_` 回到 1,后面继续 `add` 会再涨到 32 再坍缩,所以大量 `add` 的过程中 `count_` 在 1..32 之间振荡。第一版测试写过 `assert(count == 1)`,跑 37 次 `add` 后实际是 5,挂了。正确的不变量是 `count <= kMaxRects`(有界)+ `bounds()` 覆盖所有已加矩形(全跨度不丢),而不是某个会被振荡打破的中间值。

有了脏区,显示路径就分成了两步。第一步 `composite()`:照常把桌面/窗口/光标全帧画进 back buffer,但**不再**像旧 `Canvas::flip()` 那样把整帧推到帧缓冲:

```cpp
void WindowManager::composite() {
    ...
    draw_cursor(*screen_);
    // F13 §4c: the frame is NOT presented here. The cinux::gui pump flushes the dirty
    // region to the host (which forwards to the framebuffer) after composite() returns.
}
```

（`window_manager.cpp:204`。)第二步 flush:`pump()` 读出这一帧的脏矩形集合,只把每个脏矩形覆盖的 staging 像素经 Host 表的 `flush` 回调推走。`cinux_flush()` 在 adapter 里按帧缓冲自己的 pitch,把每个脏矩形的对应行从 `back_buffer()` 搬进 volatile MMIO——同样的像素,只搬变化的部分。

这里 `Canvas` 起的就是那座**桥**的作用:它的 `back_buf_` 是合成器写入的 staging 表面,`front_buf_` 指向硬件帧缓冲。core 通过 Host 表拿到「staging 基地址 + 脏矩形」,不直接碰 MMIO;`Canvas` 暴露的 `back_buffer()`(`canvas.hpp:215`)就是给 adapter 拿 staging 用的。两边各管一摊:core 管合成和几何,host 管怎么把像素真正送上网线/总线。

「哪些算脏」是个策略,放在宿主的 `cinux_render_frame` 里:光标移动(最高频)只标新旧两个光标足迹;开窗/销窗/抬升/拖动这种结构变化标全屏(z-order 一变就是大面积曝光);终端有新输出也标全屏;啥都没动就报 `count == 0`,`pump()` 这一帧零推送。

## 事件枚举撞名,与一段重复代码

### 事件过线:`EventType` → `EventCode`

把 GUI core 抽进 `cinux::gui` 这个命名空间时,撞了一个 C++ 的硬规则:**命名空间可以追加,但符号名不能重定义**。

core 的核心事件枚举原来叫 `EventType`。审计了一遍——`Rect`、`Region`、`pump`、`Surface` 在内核里要么是注释命中、要么根本不存在,都不撞。唯独 `EventType`:内核 `kernel/gui/event.hpp:30` 早就有一个 `cinux::gui::EventType`(`MouseMove`/`MouseDown`/.../`KeyDown`/`KeyUp`)。同一个命名空间里两个 `enum class EventType`,编译器直接报重定义。

解决办法是把 core 那个改名 `EventCode`(`event.hpp:18`,`kPointer`/`kKeycode`/`kEncoder`/`kTouch`),内核那个 `EventType` 不动。这俩其实不是一层东西:内核 `EventType` 是**已解码、带语义**的判别值(鼠标移动、按下、键盘按下);core 的 `EventCode` 是**线格式**的大类(这是个指针事件、这是个键码事件),细节(按下还是抬起)放在 payload 里。

这层「线格式」是有意为之。core 定义了一个 8 字节定宽头 `EventHeader{magic, version, type, flags, payload_len}` + 变长 payload:头里的 `magic`/`version` 是字节序和 ABI 版本的哨兵,`payload_len` 把可读范围钉死,ABI 错配也不会越界读。宿主侧的 `poll_event` 把内核 `EventType` **序列化**成这套头 + payload,`dispatch_event` 再**反序列化**回去。明明在同一个内核里,为什么要绕这一去一回?不是为了性能,而是为了让 `pump()` 真的中性——它只认 `EventHeader`,不认 `cinux::gui::Event`,未来换了宿主、事件源变了,`pump()` 一行不改。

> 顺带一个踩过的坑:序列化和反序列化必须**对称**。`poll_event` 里 `switch(EventType)` 和 `dispatch_event` 里 `switch(EventCode)` 是一对,给事件表加一个新类型,这两边必须同时改,否则事件会被悄悄 drop(`default` 分支返回 false,不崩但丢事件)。源码里 `default` 分支有注释专门警告这一点。

### `launch_user_program`:收敛两段一样的「建栈 + 跳用户态」

GUI 里有一段「把程序加载进用户态」的代码——`execve` 之后,预映射用户栈、记一个 demand-growth 的栈 VMA(一个挡住栈越界落到无 VMA 区直接 segfault 的硬门控,前几章立过)、激活地址空间、`jump_to_usermode`。这段在解耦前有**两份几乎一字不差**的拷贝:一份在 `init.cpp` 非 GUI 的 shell 启动路径里,一份在 `gui_init.cpp` 的 `shell_child_entry` 里(后者多一步给 `fd_table` 接 stdin/stdout 管道)。

把这段「加载 + 跳」收敛成共享函数,动机不是整洁,而是堵住第三份拷贝:host adapter 的 `spawn` 迟早要再走一次同样的路径,不先合并就会有三个一字不差的临界区分头演进——改一处忘改另两处,是迟早的事。于是把它收敛成一个 `launch_user_program`:

```cpp
namespace cinux::proc {
/// execve() + user stack setup + jump to user mode (never returns)
void launch_user_program(const char* path, const char* const argv[], const char* const envp[]);
}
```

（`user_launch.hpp`。）它「永不返回」:任何一步失败就 `exit_current` 兜底。注意它叫 `launch`(加载 + 跳)不叫 `spawn`(建进程)——task 的创建是 `fork`/`TaskBuilder` 的事,调用方负责 `new AddressSpace`(GUI shell 还额外负责接 `fd_table`),`launch_user_program` 只管「最后那一跳」。GUI 侧和 init 侧现在共用这一个函数,调用点都瘦成了一行 `cinux::proc::launch_user_program(...)`。

## 诚实的边界:哪些还没做完

像 051 那样,把没做完的事说清楚,免得读者把骨架当成成品。

**软件光栅化(`swraster`)只是骨架,没接管合成。** `core/swraster.cpp` 这次确实编进了 `cinux-gui` 库,有一组单元测试在用(fill/blit/blend/glyph),但 `WindowManager::composite()` 目前还在走自己旧的绘制路径,没接 `swraster`。所以「core 的光栅化引擎真正接管渲染」是后续的事——眼下 core 只负责经 Host 表驱动 input / render_frame / flush 这条路径,绘制本身还在内核侧。别看着 `swraster` 在库里就以为它已经画屏幕了。

**`spawn` 只是个桌面壳。** Host 表的 `HostDesktop.spawn` 目前无视 `path`/`argv`,直接转去 `create_shell_terminal()`——也就是说,今天只有「点桌面图标开一个 shell」这一个动作。通用的 `spawn(path, argv)` 返回真实 stdio 句柄,是后面的工作。

**源码里还留着 `#ifdef CINUX_GUI`。** core 虽说拔出来了,但 `init.cpp` 里 GUI 启动 / 非 GUI 启动仍然是 `#ifdef CINUX_GUI ... #else fork+exec ... #endif` 这种「读到一半分叉两条路」的写法。把这种源码级开关**全归到 CMake 的文件级 gate**(同一个接口两份实现,CMake 选编一份,调用处一行零 `#ifdef`),再补一组 USB 空壳让 GUI 在真 xHCI 驱动落地前也能链接——这套「解耦收尾」时间上排在 xHCI 驱动之后,留到后面。所以本章的 GUI worker 线程目前还内联在 `init.cpp` 里,没有独立的 `desktop_launch` 文件;这是边界,不是遗漏。

**core 实际只服务 PC 这类宿主。** 它虽然写得 freestanding 友好(只用 `<stdint.h>`/`<stddef.h>`、整数 only),但 `host.hpp` 注释明确写「PC hosts only」——没走 MCU 那条路。所以别被它 freestanding 的样子骗了,它面向的只有 PC 一种后端;那些能在禁堆上下文跑的特性,只是顺带的好性质,不是为别的宿主准备的。

验证该看到什么,见配套 lab。下一章 055 接 xHCI USB——那一步落地后,GUI 输入才有真 USB 可走;再之后,上面留的「解耦收尾」才轮得上做。
