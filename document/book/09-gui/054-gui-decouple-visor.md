---
title: 054 · GUI 解耦:host-neutral core、一张 Host 表,和 userspace host 进程
---

# 054 · GUI 解耦:host-neutral core、一张 Host 表,和 userspace host 进程

> 029 到 033 在内核里搭起了一整套桌面:双缓冲画布、窗口管理器、位图图标、终端。但那套 GUI 当时是**焊死在内核里**的——刷新挂在时钟中断回调里,合成完直接写帧缓冲,任何一笔改动都得把整个内核跑起来才能看见。这一章把它整个搬出来:GUI 的核心几何/事件/光栅化逻辑抽成一个**不 include 任何内核头**的独立库,内核这边连一个 host 适配单元都不剩;真正驱动桌面的是一个**普通的 ring3 进程** `/cinux_gui_host`,它打开 `/dev/fb0` 画像素、读 `/dev/event0` 拿输入,合成和事件泵全在用户态跑。内核只剩两个文件:一个在 ISR 里把鼠标键盘事件推进 `/dev/event0`,一个 fork+execve 把 host 进程拉起来。
>
> 一条诚实的边界先说在前头:这一章讲的是**架构骨架**——host-neutral core 怎么立、Host 表怎么填、内核薄接缝留到多薄。host 进程的渲染路径里目前有一条**遗留的防御性 workaround**:`host_render_frame` 每帧都报一个全屏脏矩形,把脏矩形优化实际短路掉。这条 workaround 当年是用来补一个 core bug 的,但**那个 core bug 早已修掉**(下面会讲到为什么 host 这边的全屏 flush 留着没拆),拆它是个小活但本章没做。

## 这章咱们要点亮什么

1. **被焊死的桌面长什么样**:刷新在时钟中断里、合成直写帧缓冲、GUI 代码和内核搅在一起——以及为什么这样既不可靠也不能动。
2. **Host ABI 表**:host-neutral core 和宿主之间**唯一**的硬接缝,一张函数指针表。谁填表,core 就跑在谁上面。
3. **host-neutral core**:不 include 任何内核头、不带 vtable/RTTI/异常,只用 `<stdint.h>`/`<stddef.h>`。同一份代码,host 填上表就能在内核测试里跑、在 Linux fbdev 上跑、在 SDL 窗口里跑、在 Cinux 用户态进程里跑。
4. **userspace host 进程**:`/cinux_gui_host` 是一个普通的 ring3 ELF,`open`/`mmap`/`read`/`poll`/`fork`/`execve` 全是 syscall,跟 `/bin/sh` 没本质区别。它填 Host 表、循环调 `core->pump()`。
5. **内核薄接缝**:整个 `kernel/gui/` 只剩 `gui_init.cpp`(ISR 里 dual-write 事件到 `/dev/event0`)和 `desktop_launch.cpp`(fork 出 host 进程),旧的内核 host 适配器已删。
6. **Region 永不欠覆盖**:脏矩形集合容量溢出时坍缩成包围盒——多推几个没变的像素是性能损失,漏推一个变了的像素是视觉 bug。

## 被焊死的桌面:刷新挂在时钟中断里

先看要修的「病」。在 029–033 那套 GUI 里,屏幕刷新是这么驱动的:向 PIT 注册一个回调,每次 IRQ0 时钟中断就在 **ISR 上下文里**排空鼠标键盘事件、合成一帧、把整帧 `flip()` 到帧缓冲。这是「内核/中断把刷新推给 GUI」的模型。

这个模型有两个病根,而且一个比一个要命。

第一个,**在中断里跑渲染本身就是雷区**。中断上下文里任何意外的阻塞——合成时撞上一个还没就绪的状态、某条路径多绕了一下——都会把整条 IRQ 路径钉死,表现就是卡屏或黑屏。中断是「要尽快交还」的上下文,在里面干一整帧的活本来就违和。

第二个更隐蔽。把刷新押在 PIT tick 的到达上,等于押在一个**在另一种中断路由配置下可能根本不反复触发**的硬件特性上——当内核切到 APIC 路由后,legacy PIT 那条 IRQ0 的投递行为会和 8259 PIC 下不一样。也就是说,「每次时钟中断刷新一帧」这个假设本身就很脆。

所以要做的事,不是给旧路径打补丁,而是换一个模型。但换模型之前得先解决一个前置问题:GUI 代码现在和内核搅在一起,无论把它放进哪个线程都还是内核态那一坨,放进哪个中断都还是 ISR 上下文。于是第一步是**把 GUI 从内核里整个拔出来**——拔到一个连内核头都不 include 的库里,拔到连「跑在哪条特权级上」都不自知。

## 立一道硬边界:host-neutral core

拔出来的办法,是给 GUI 立一道**硬边界**:边界这边是「host-neutral core」——只懂 GUI 的事(矩形代数、事件泵、光栅化、控件树),不认识 framebuffer、不认识 IRQ、不认识进程结构、不认识 syscall;边界那边是「宿主」——今天是 Cinux 的 userspace host 进程,明天可能是 SDL 模拟器,后天可能是个 offscreen 测试驱动。两边之间**只通过一张表**说话。

这张边界在文件层就看得见。core 全部住在 [`third_party/Cinux-GUI/core/`](third_party/Cinux-GUI/core/gui_core.hpp),它的核心会话类 `GuiCore` 的源文件 [`gui_core.cpp`](third_party/Cinux-GUI/core/gui_core.cpp) 顶部写得很直白:

> Host-neutral: ZERO host includes. Owns the staging buffer; render_frame paints into it; region algebra collects the dirty rects; flush pushes each to the host. See gui_core.hpp for the flush-display-model contract.
>
> ([`gui_core.cpp:5`](third_party/Cinux-GUI/core/gui_core.cpp#L5))

注意这句 "ZERO host includes" 不是修辞。看 [`core/`](third_party/Cinux-GUI/core/) 下任何 `.cpp` 的 `#include` 段,出现的全是 `<stdint.h>` + 同目录的兄弟头(`host.hpp`/`region.hpp`/`event.hpp`)——**没有一个内核头,也没有一个 Linux 头**。`core/` 因此能用普通的 g++/clang++ 直接编,跑 ctest,完全不需要把内核起来。

怎么证明它真的 host-neutral?子模块里带了好几个**零目标平台**的 host 程序,每个都是一份独立的表填充:[`host/widgets_host_main.cpp`](third_party/Cinux-GUI/host/widgets_host_main.cpp) 把控件树渲一帧到 malloc 的缓冲里、dump 成 PPM;[`host/fake_host_main.cpp`](third_party/Cinux-GUI/host/fake_host_main.cpp) 手填一张假表(`fake_poll_event` 永远返回 false、`fake_flush` 记调用次数),照样调 `pump()`;[`host/sdl_host_main.cpp`](third_party/Cinux-GUI/host/sdl_host_main.cpp) 在一个 SDL 窗口里跑同样的控件树;[`host/linux_fbdev_main.cpp`](third_party/Cinux-GUI/host/linux_fbdev_main.cpp) 直接 mmap `/dev/fb0` + 读 `/dev/input/event*`。**同一份 core 驱动 SDL 窗口、Linux framebuffer、offscreen dump、Cinux 用户态进程,只靠换一张表的填充**——这就是 host-neutral 的可证伪证据。

## Host ABI 表:core 和宿主之间唯一的接缝

这道边界两边靠什么通话?靠一张纯数据的函数指针表。它在 [`core/host.hpp`](third_party/Cinux-GUI/core/host.hpp) 里,叫 `Host`:

```cpp
struct HostCore {
    /* L1 显示后端:把一块脏矩形从 staging 缓冲推到显示屏(core → host)。
     * pixels 是 core 拥有的 staging 基地址,x/y/w/h 在显示坐标里定位脏矩形。 */
    void (*flush)(void* ctx, int x, int y, int w, int h, const void* pixels, uint32_t stride,
                  PixelFormat fmt);
    void (*flush_complete)(void* ctx); /* host → core:上一次异步 flush 完成 */

    /* L2 输入后端:从队列里取一个事件,空了返回 false */
    bool (*poll_event)(void* ctx, EventHeader* out, uint16_t out_cap);

    /* L4 把取出来的事件应用到宿主自己的 GUI 状态上;合成一帧进 staging,
     * 报告哪些矩形脏了。core 永远看不到 host 的 GUI 类型。 */
    void (*dispatch_event)(void* ctx, const EventHeader* ev, const void* payload);
    void (*render_frame)(void* ctx, Frame* frame);

    uint32_t (*now_ms)(void* ctx);          /* 时间 */
    void* (*alloc)(void* ctx, size_t n);    /* 内存 */
    void (*free)(void* ctx, void* p);
    void (*log)(void* ctx, const char* fmt, ...);
};

struct HostDesktop {   /* 桌面扩展:起进程。不能 spawn 的宿主填 nullptr */
    int (*spawn)(void* ctx, const char* path, char* const argv[], int* stdin_fd, int* stdout_fd);
};

struct Host {
    HostCore     core;
    HostDesktop* desktop; /* nullptr on hosts without spawn */
    void*        ctx;     /* 透传给每个回调的不透明上下文 */
};
```

（[`host.hpp:56`](third_party/Cinux-GUI/core/host.hpp#L56) 起。）这张表里全是**函数指针**——core 永远不直接调 framebuffer 或鼠标队列,它调的是 `host->core.flush(...)` 这种间接调用。这就引出这一整弧最关键的一句话:**谁填这张表,core 就跑在谁上面**。

> **为什么是函数指针表,不是 C++ 虚函数/抽象基类?** 因为表是纯数据、不带 vtable/RTTI/异常这些「特权包袱」。core 因此能保持 freestanding 友好——只用 `<stdint.h>`/`<stddef.h>`,任何 hosted 编译器都能编。表项允许 `nullptr`,`pump()` 对每个回调先判空再调,所以**半填的表也是安全的**:不能 spawn 的宿主,`desktop` 填 `nullptr` 就行,不会逼它编一个永远没人调的空函数。这条性质后面会反复用到——Cinux 的 userspace host 就把 `desktop` 填成 `nullptr`,因为它自己在用户态用 `fork`/`execve` 起 shell,根本用不到 core 替它 spawn。

core 和 host 之间还有一条**数据流向的硬规矩**,叫 flush 显示模型,写在 `Frame` 结构的注释里:

```cpp
struct Frame {
    Rect*       rects;
    uint32_t    max_rects;
    uint32_t    count;       /* host 写入:host 这一帧报告了几个脏矩形;0 = 空闲 */
    void*       pixels;      /* core 拥有的 staging 基地址;host 在这里画 */
    uint32_t    stride;
    uint32_t    width;
    uint32_t    height;
    PixelFormat format;
};
```

（[`host.hpp:42`](third_party/Cinux-GUI/core/host.hpp#L42)。)注意 `pixels`/`stride`/`width`/`height`/`format` 全是 **core 预填好的**——staging 缓冲的归属权在 core 这边,host 只是被邀请进来「在这块缓冲上画」。host 写回的只有 `rects[]` 和 `count`:这一帧它弄脏了哪几块。**core 拥 staging,host 只收脏帧**——这是 flush 显示模型的核心,它让 core 不用关心 host 怎么画、画给谁看。

## GuiCore::pump():不感知自己在哪条道上的主循环

core 那一侧到底拿这张表干什么?全在 `GuiCore::pump()` 一个函数里。它是 GUI 主循环的**一圈**,函数体里没有任何宿主头文件,只做固定四步:

```cpp
void GuiCore::pump() {
    if (host_ == nullptr || staging_.pixels == nullptr) {
        return;
    }
    HostCore& hc = host_->core;

    /* 1. 排空输入:poll_event 取一个、dispatch_event 派一个,直到队列空 */
    if (hc.poll_event != nullptr && hc.dispatch_event != nullptr) {
        alignas(uint32_t) uint8_t buf[sizeof(EventHeader) + kMaxPayload];
        auto*                     hdr     = reinterpret_cast<EventHeader*>(buf);
        const void*               payload = buf + sizeof(EventHeader);
        while (hc.poll_event(host_->ctx, hdr, sizeof(buf))) {
            hc.dispatch_event(host_->ctx, hdr, payload);
        }
    }

    /* 2. 预填 Frame,然后请宿主往 staging 上画一帧 + 报告脏矩形 */
    if (hc.render_frame == nullptr || hc.flush == nullptr) {
        return;
    }
    Frame frame{};
    frame.rects     = dirty_rects_;
    frame.max_rects = kMaxDirtyRects;
    frame.pixels    = staging_.pixels;  /* CORE owns staging -> host paints HERE */
    frame.stride    = staging_.stride_bytes;
    frame.width     = staging_.width;
    frame.height    = staging_.height;
    frame.format    = staging_.format;
    hc.render_frame(host_->ctx, &frame);

    if (frame.count == 0u) {
        return;  /* idle:render_frame 返回后立刻判 count,空帧连一个像素都不 flush */
    }

    /* 3. 把 host 报告的脏矩形过一遍 Region(去重 / 溢出坍缩) */
    Region reg;
    for (uint32_t i = 0u; i < frame.count; i++) {
        reg.add(frame.rects[i]);
    }

    /* 4. 把每个 region 矩形从 staging 推给显示后端 */
    const uint32_t n = reg.count();
    for (uint32_t i = 0u; i < n; i++) {
        const Rect& r = reg.rects()[i];
        hc.flush(host_->ctx, r.x0, r.y0, r.x1 - r.x0, r.y1 - r.y0, staging_.pixels,
                 staging_.stride_bytes, staging_.format);
    }
}
```

（[`gui_core.cpp:44`](third_party/Cinux-GUI/core/gui_core.cpp#L44),idle 跳过在 [`:79`](third_party/Cinux-GUI/core/gui_core.cpp#L79)。) `pump()` 自己**做不了任何 GUI 的事**——它没有窗口管理器、没有渲染器、不知道屏幕在哪。它只是个协议骨架:排空输入、把 staging 缓冲递给 host 让它画、把 host 报告的脏区收拢、推走。整个函数对宿主是什么类型一无所知,这就是「不感知自己跑在内核态还是用户态」的实现机制。

注意第 2 步尾部那个 idle 跳过:`render_frame` 返回后立刻判 `frame.count == 0`(这一帧什么都没变),`pump()` 直接 `return`,**连一个像素都不 flush**。这是后面「空闲不空转」的关键——前提是 host 那一侧能准确报告脏区。Cinux 的 userspace host 现在还没做到这点,下面讲。

## Region:脏矩形代数,永不欠覆盖

第 3 步把 host 报告的脏矩形收拢,用的是 core 自己的 `Region`——一个**定容**的矩形集合。先说为什么需要它,再说它的硬规矩。

一帧里「变了」的往往不止一块:鼠标挪一格、终端吐一个字符。如果每帧都把整屏(1024×768,约 3 MB)从 staging 整块搬到帧缓冲,绝大多数搬运是浪费——帧缓冲走的是 MMIO 总线时尤其浪费。脏矩形的思路:渲染照常全帧画进 staging(保证内容恒新),但只把**这一帧变了的那几块**矩形覆盖的像素推给显示端。

Cinux 给这套几何立了一条硬规矩,写在 [`region.hpp`](third_party/Cinux-GUI/core/region.hpp#L10) 顶部:**宁可多推,绝不漏推**。多推几个没变的像素,是看不见的性能损失;漏推一个变了的像素,是屏幕上残留旧内容的视觉 bug。这条规矩贯穿下面所有的设计。

脏矩形运算建立在**半开区间**的 `Rect` 上:`[x0, x1) × [y0, y1)`,左/上含、右/下不含。一个宽 4 的矩形写成 `{0,0,4,4}`,覆盖的是第 0、1、2、3 列(不是 4 列)。这样两个相邻矩形 `{0,0,4,4}` 和 `{4,0,8,4}` 无缝拼接——共享的那条边 `x=4` 只属于右边那个,不重叠也不漏缝。退化(空)矩形的判据也就干净了:`x0 >= x1 || y0 >= y1`（[`region.hpp:47`](third_party/Cinux-GUI/core/region.hpp#L47))。

一帧里变了的多块用 `Region` 累积——一个定容的矩形集合:

```cpp
class Region {
public:
    static constexpr uint32_t kMaxRects = 32;
    void add(const Rect& r);
    /* ... */
private:
    Rect     rects_[kMaxRects];
    uint32_t count_ = 0;
};
```

（[`region.hpp:113`](third_party/Cinux-GUI/core/region.hpp#L113) 起,容量常量 `kMaxRects = 32` 在 [`:116`](third_party/Cinux-GUI/core/region.hpp#L116)。)定容、`add()` 不分配堆——这是为了让最常见的「一帧累积脏区」这条热路径能在任何上下文里跑。那塞到第 33 个怎么办?它不报错也不扩容,而是把当前所有矩形 + 新矩形的**包围盒**算出来,整个 `Region` 坍缩成这一个矩形:

```cpp
void Region::add(const Rect& r) {
    if (r.empty()) {
        return;
    }
    if (count_ < kMaxRects) {
        rects_[count_++] = r;
        return;
    }
    /* Overflow: collapse the whole region to its bounding box (all current
     * members plus the new rect). Over-approximation, never under-cover. */
    Rect bbox        = bounds();
    bbox             = rect_union(bbox, r);
    count_           = 0;
    rects_[count_++] = bbox;
}
```

（[`region.cpp:51`](third_party/Cinux-GUI/core/region.cpp#L51)。)这就是「宁可多推绝不漏推」的落地:包围盒盖住了所有原本的矩形(可能也盖了些本来没变的像素),但绝不会漏掉任何一个变了的像素。对脏区来说,过覆盖是性能问题,欠覆盖才是正确性问题,所以一律取前者。

> **一个真实的坑,别照搬「不分配堆」当不变量。** `add()` 确实不分配堆(热路径,任何上下文能跑),但同类的 `Region::subtract()` 不一样——它会 `new Rect[kMaxRects*4]` 临时缓冲(`region.cpp:92`)。原因是减法能把一个矩形切成最多 4 块,32 个矩形一减就 128 个,在内核栈上撑不住。注释专门标了这条「heap-allocated: Rect[kMaxRects*4] (128 rects ~= 2 KB) on the stack busts a tight kernel stack」。要说清楚的是:`subtract` 目前只是 `Region` 暴露给将来 occlusion / 切割路径的 API,**core 现在的渲染路径还没调它**(`pump()` 第 3 步只调 `add`,widget/compositor 里也没有 subtract 调用点)——但它的实现确实上堆,所以「`Region` 整个类零分配」这个直觉是错的。记住 add 零分配、subtract 上堆,就够了。

## userspace host 进程:一个普通的 ring3 程序

讲完 core 和表,该看真正驱动桌面的那个进程了。Cinux 的 userspace host 住在 [`user/cinux_gui_host/main.cpp`](user/cinux_gui_host/main.cpp),它就是一个普通的 ring3 ELF——`open`/`mmap`/`read`/`poll`/`fork`/`execve` 全是 syscall,跟 `/bin/sh` 没有本质区别。

它干的活用一句话讲完:**打开 `/dev/fb0` 拿到画布、打开 `/dev/event0` 拿到输入、填一张 Host 表、循环调 `core->pump()`**。分三段看。

**第一段,main 里把设备和 GUI 会话立起来**（[`main.cpp:486`](user/cinux_gui_host/main.cpp#L486) 起):

```cpp
int fb_fd = open("/dev/fb0", O_RDWR);
if (fb_fd < 0) {
    return 1;
}
struct fb_screen_info info;
if (ioctl(fb_fd, FBIOGET_SCREENINFO, &info) != 0) {
    return 2;
}
size_t   sz = static_cast<size_t>(info.pitch) * static_cast<size_t>(info.height);
uint8_t* fb =
    static_cast<uint8_t*>(mmap(nullptr, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0));
if (fb == reinterpret_cast<uint8_t*>(-1)) {
    return 3;
}
int ev_fd = open("/dev/event0", O_RDONLY);
```

`/dev/fb0` 是内核暴露的帧缓冲设备(087 详讲),`ioctl(FBIOGET_SCREENINFO, ...)` 拿到宽高 pitch,`mmap(... MAP_SHARED ...)` 把它映射进自己的地址空间——往后往这块内存写像素,就是往屏幕写像素。`/dev/event0` 是内核暴露的输入事件设备(也是 087),host 用普通的 `read` 从里面取鼠标键盘事件。这两个 fd 就是 host 和硬件之间**全部**的接缝。

接着建控件树(WindowManager、几个 Window、DesktopIcon、TerminalWidget),然后填表:

```cpp
Host host{};
host.core.flush          = host_flush;
host.core.poll_event     = (ev_fd >= 0) ? host_poll_event : nullptr;
host.core.dispatch_event = (ev_fd >= 0) ? host_dispatch_event : nullptr;
host.core.render_frame   = host_render_frame;
host.core.now_ms         = host_now_ms;
host.core.alloc          = host_alloc;
host.core.free           = host_free;
host.core.log            = host_log;
host.desktop             = nullptr;   /* host 自己用 fork/execve 起 shell,不需要 core 替它 spawn */
host.ctx                 = &st;

auto* core = new GuiCore(&host, info.width, info.height, PixelFormat::kXrgb8888);
```

（[`main.cpp:558`](user/cinux_gui_host/main.cpp#L558)。)看清楚:除了 `desktop` 填 `nullptr`(host 自己会起 shell,用不到 core 替它 spawn),其他每个回调都指向这个文件里实现的 `host_*` 函数。`GuiCore` 构造时拿到了 staging 缓冲的所有权——它会按 `width*height*4` 分配一块内存(看 [`gui_core.cpp:32`](third_party/Cinux-GUI/core/gui_core.cpp#L32) 的 `new uint8_t[...]`),以后每帧 host 就往这块内存上画。

**第二段,主循环**(就这两行,[`main.cpp:573`](user/cinux_gui_host/main.cpp#L573)):

```cpp
unsigned long iter = (n_pump == 0) ? ~0UL : n_pump;
for (unsigned long i = 0; i < iter; ++i) {
    core->pump();
}
```

`argv[1]` 解析成 `n_pump`:`"0"` 表示无限(取 `~0UL`),数字表示跑几圈后退出——后者是个**冒烟自检**:跑完指定圈数后,代码会读屏幕中心那个像素是不是非零([`main.cpp:578`](user/cinux_gui_host/main.cpp#L578) 起),黑屏返回 5。整个桌面就靠这个 `for` 撑着——没有 PIT 回调、没有内核线程、没有 IRQ,就是一个普通进程在用户态反复 pump。

**第三段,看那几个 `host_*` 回调具体干什么**——这才是 host-neutral core 真正落地的地方。

输入侧 `host_poll_event`（[`main.cpp:220`](user/cinux_gui_host/main.cpp#L220)):`poll(ev_fd, timeout=0)` 非阻塞探一下,有数据就 `read` 一个 `kernel_event` 出来,把它的 `type_` 字段翻译成 core 的 `EventCode` + 对应 payload,塞进 `EventHeader` 缓冲:

```cpp
bool host_poll_event(void* ctx, EventHeader* out, uint16_t cap) {
    auto* st = static_cast<HostState*>(ctx);
    if (out == nullptr || st->ev_fd < 0 || cap < sizeof(EventHeader)) {
        return false;
    }
    struct pollfd pfd;
    pfd.fd      = st->ev_fd;
    pfd.events  = POLLIN;
    pfd.revents = 0;
    // Nonblocking (timeout=0): pump stays fully responsive -- mouse/keyboard
    // events are read with no latency.  A 10ms blocking wait was tried (to
    // release CPU during gcc compiles) but reverted: it didn't fix the real
    // stall (NVMe sync poll under NvmeBlockDevice::lock_, can't yield) and
    // only added input lag.  The compile-time stall is a known v1.0.0 issue;
    // the cure is async IO (v1.1+).
    if (poll(&pfd, 1, 0) <= 0) {
        return false;
    }
    kernel_event kev;
    ssize_t      n = read(st->ev_fd, &kev, sizeof(kev));
    if (n != static_cast<ssize_t>(sizeof(kev))) {
        return false;
    }
    out->magic = kEventMagic; out->version = kAbiVersion; out->flags = 0;
    auto* tail = reinterpret_cast<uint8_t*>(out) + sizeof(EventHeader);
    switch (kev.type_) {
    case kMove: case kMouseDown: case kMouseUp: {
        /* 写 payload 前先查 cap,确保 tail 缓冲放得下 */
        if (cap < sizeof(EventHeader) + sizeof(PointerPayload)) { return false; }
        out->type = EventCode::kPointer;
        PointerPayload p;
        p.kind = (kev.type_ == kMouseDown) ? kPointerKindDown
                : (kev.type_ == kMouseUp) ? kPointerKindUp : kPointerKindMove;
        p.x = kev.mouse.x; p.y = kev.mouse.y;
        p.dx = kev.mouse.dx; p.dy = kev.mouse.dy; p.buttons = kev.mouse.buttons;
        memcpy(tail, &p, sizeof(p));
        out->payload_len = sizeof(p);
        break;
    }
    case kKeyDown: case kKeyUp: {
        /* 同样查 cap 后填 KeycodePayload(此处省略) */
        out->type  = EventCode::kKeycode;
        out->flags = (kev.type_ == kKeyDown) ? kEventFlagPressed : 0;
        break;
    }
    default: return false;
    }
    return true;
}
```

注意这一去一回的**序列化**:内核那边把鼠标键盘事件塞进 `/dev/event0` 时用的是自己的 `kernel_event` 结构(就是 [`main.cpp:97`](user/cinux_gui_host/main.cpp#L97) 镜像的 `cinux::gui::Event` 内存布局),host 这边读出来再翻译成 core 的 `EventHeader` + `PointerPayload`/`KeycodePayload`。为什么要绕这一去一回?不是为了性能,而是为了让 `pump()` 真的中性——它只认 `EventHeader`,不认 `cinux::gui::Event`,换了输入源(比如换成 USB HID、换成 SDL 的事件)这行代码不用改,改 `host_poll_event` 一个函数就行。

那行被注释保留的「`A 10ms blocking wait was tried ... but reverted`」也值得读:它是一个真实的**折腾记录**。曾经想让 host 在 poll 上阻塞 10ms,好让 gcc 编译时腾出 CPU;试了发现没用——真正的卡是 NVMe 驱动在 `NvmeBlockDevice::lock_` 上同步 poll 不能 yield,这个阻塞改掉了输入延迟却没解决卡顿,于是回滚。留这条注释,就是给后面接手的人省一次同样的弯路。

显示侧 `host_flush`（[`main.cpp:145`](user/cinux_gui_host/main.cpp#L145)):收到 core 推过来的脏矩形,把 staging 缓冲里对应那块像素 memcpy 进 mmap 来的 framebuffer:

```cpp
void host_flush(void* ctx, int x, int y, int w, int h, const void* pixels, uint32_t stride,
                PixelFormat fmt) {
    auto* st = static_cast<HostState*>(ctx);
    if (st->fb == nullptr || pixels == nullptr || w <= 0 || h <= 0) { return; }
    if (fmt != PixelFormat::kXrgb8888) { return; }
    /* ... clip 到屏幕边界 ... */
    const uint32_t  src_stride_px = stride / 4u;
    const uint32_t  dst_stride_px = st->fb_pitch / 4u;
    const uint32_t* src = static_cast<const uint32_t*>(pixels);
    uint32_t*       dst = reinterpret_cast<uint32_t*>(st->fb);
    for (int row = 0; row < h; ++row) {
        const uint32_t py = static_cast<uint32_t>(y + row);
        const uint32_t* srow =
            src + static_cast<size_t>(py) * src_stride_px + static_cast<uint32_t>(x);
        uint32_t* drow = dst + static_cast<size_t>(py) * dst_stride_px + static_cast<uint32_t>(x);
        for (int p = 0; p < w; ++p) {
            drow[p] = srow[p];
        }
    }
}
```

注意 `src` 用的是 core 拥有的 staging 基地址、`dst` 用的是 mmap 来的 framebuffer——这正是 flush 显示模型在硬件那头的落地:core 不知道 framebuffer 长什么样,host 不知道 staging 怎么分配的,两者只在 `host_flush` 的参数表上交班。

合成侧 `host_render_frame`（[`main.cpp:187`](user/cinux_gui_host/main.cpp#L187)):先把每个活跃 shell 的 PTY 输出 drain 进对应的 `TerminalWidget`,然后调 `desktop.render(staging, font, &dirty)` 把控件树画进 core 拥有的 staging 缓冲,再把 dirty 报告回 `frame->rects`:

```cpp
void host_render_frame(void* ctx, Frame* frame) {
    auto* st = static_cast<HostState*>(ctx);
    if (frame == nullptr) { return; }
    /* drain 每个 shell 的 PTY → TerminalWidget */
    char buf[512];
    for (uint32_t i = 0; i < HostState::kMaxShells; ++i) {
        if (st->shells_[i].master_fd < 0) { continue; }
        ssize_t n;
        while ((n = read(st->shells_[i].master_fd, buf, sizeof(buf))) > 0) {
            st->shells_[i].term.write(buf, static_cast<uint32_t>(n));
        }
    }
    Surface s{frame->pixels, frame->width, frame->height, frame->stride, frame->format};
    Region  dirty;
    st->desktop.render(s, st->font, &dirty);
    /* defensive workaround: core 早年 remove_window 不标脏,host 用每帧全屏 flush
     * 兜底。那个 core bug 已经修掉(remove_window 现在会 invalidate 关窗足迹),
     * 但 host 这条全屏 flush 留着没拆——拆它需要实跑验证关窗路径真不留残影,
     * 本章没做,是已知遗留,见章末「这章没做的」。*/
    if (frame->max_rects >= 1u) {
        frame->rects[0] = Rect{0, 0, static_cast<int32_t>(frame->width),
                               static_cast<int32_t>(frame->height)};
        frame->count    = 1u;
    }
}
```

这一段就是前面说的「已知遗留」:host 现在每帧都报一个全屏脏矩形,把 `pump()` 第 2 步尾部那个 idle 跳过和第 3 步的 Region 收拢实际短路掉了。**这件事的来龙去脉值得说清楚**——它不是「core 还没修」的现况,而是一条**遗留的防御性 workaround**:当年 core 的 `WindowManager::remove_window` 关窗时不标脏,host 只能每帧全屏 flush 兜底(否则关掉的窗口会在屏幕上留残影,直到光标划过那块区域才被覆盖)。但那个 core bug **早已修掉**——现在的 `remove_window` 在 unlink 之前先存下关窗足迹 `stale`,然后 `invalidate(stale)`（[`window_manager.cpp:39`](third_party/Cinux-GUI/core/widget/window_manager.cpp#L39)),连 `add_window`/`add_icon`/光标移动/Z-order 变化也都各自 invalidate。源码注释自己也写:「This was a core bug -- hosts worked around it with a full-screen dirty flush each frame, but the proper fix is invalidating here, mirroring add_window()」。

也就是说:**core 这一侧已经能正确报告脏区了**,host 全屏 flush 在原理上已经不再必要。但 host 那条 `frame->count = 1` 的全屏分支留着没拆,因为拆它需要实跑验证「关一个窗口、看屏幕真不留残影」,这一步本章没做,留到后面(见「这章没做的」)。读者看到这里别以为脏矩形机制没用上:几何代数(Region、半开区间 Rect、`remove_window` 的 `invalidate`)是 core 真的在跑的,只是 host 这一侧目前故意报全屏——把它拆回报真脏区,是个范围明确的收尾活,不是设计缺陷。

> **同一个 main 怎么既能编成 freestanding 友好的 host、又能用 syscall?** 因为它静态链接 musl,syscall 走 musl `libc.a` 的薄包装。看构建脚本 [`tools/musl/build-cinux-gui-host.sh`](tools/musl/build-cinux-gui-host.sh):`g++ -static -nostdlib -no-pie -fno-rtti -fno-exceptions -std=c++17`,链接 21 个 core 源 + `main.cpp` + 一个 [`crt_stub.cpp`](user/cinux_gui_host/crt_stub.cpp)。`-nostdlib` 不链默认运行时,改用 musl 的 `Scrt1.o`/`crti.o`/`crtn.o` + `-lc -lgcc`;core 是 freestanding C++(没有 STL、没有异常、没有 RTTI),所以一个只含 `operator new/delete → malloc/free` 桩 + `__cxa_pure_virtual → abort` 的 [`crt_stub.cpp`](user/cinux_gui_host/crt_stub.cpp) 就够,**不链 libstdc++**。整个 ELF 静态、无动态依赖,扔进 initramfs 内核 `fork+execve` 就能跑。

## 内核只剩两个文件:薄接缝长什么样

讲完 host 进程,回头看内核这边剩了多少。整个 `kernel/gui/` 子目录现在**只有三个文件**:`event.cpp`(一个老的统一事件队列,测试在用,生产输入其实走 `/dev/event0`)、`gui_init.cpp`、`desktop_launch.cpp`。后两个才是真正的「薄接缝」。看 [`kernel/gui/CMakeLists.txt`](kernel/gui/CMakeLists.txt) 怎么描述这件事:

> the in-kernel host adapter (host_cinux.cpp) is deleted -- the widget tree + Host ABI table + GuiCore pump now live entirely in the USERSPACE GUI host (user/cinux_gui_host, fork+execve'd by desktop_launch). kernel/gui/ keeps only the kernel-side plumbing the userspace host can't do:
> - event.cpp : unified mouse+keyboard EventQueue (test_mouse_event exercises it; production input reaches the host via /dev/event0, not this queue)
> - gui_init.cpp : PS/2 mouse + keyboard listener -- dual-writes each decoded key into /dev/event0 for the userspace host
> - desktop_launch.cpp: GUI-side launch_userspace (fork+execve /cinux_gui_host) + handoff_framebuffer_to_gui

——也就是说,**旧的内核 host 适配器 `host_cinux.cpp` 整个删掉了**。它当年填的那张 Host 表、它当年实现的 `cinux_flush`/`cinux_poll_event`/`cinux_render_frame` 那一整套,全挪进了 `user/cinux_gui_host/main.cpp`。内核这边不再碰 Host 表。

### gui_init.cpp:ISR 里把事件推进 /dev/event0

第一个文件,[`gui_init.cpp`](kernel/gui/gui_init.cpp),全文 62 行,只做两件事:初始化 PS/2 鼠标、注册一个键盘 listener。看 `gui_start()`:

```cpp
void gui_start() {
    cinux::lib::kprintf("[GUI] ===== Milestone 033: GUI Desktop (F13-B new core) =====\n");

    cinux::drivers::Mouse::init();
    cinux::drivers::Keyboard::register_key_listener(on_key_event);

    cinux::lib::kprintf(
        "[GUI] Mouse + keyboard listener initialised; desktop driven by the "
        "userspace GUI host (/cinux_gui_host).\n");
}
```

（[`gui_init.cpp:47`](kernel/gui/gui_init.cpp#L47)。)`Mouse::init()` 装好 PS/2 鼠标的中断处理(IRQ12),`Keyboard::register_key_listener(on_key_event)` 给键盘驱动挂一个回调。**键盘驱动本身没有任何 GUI 依赖**——它只负责解码 scancode,然后调「谁注册了 listener 就调谁」,这是 Cinux 的 §14 约定(代码味道:驱动不该硬挂上层)。`on_key_event` 是 `gui_init.cpp` 自己的文件内函数:

```cpp
void on_key_event(const cinux::drivers::KeyEvent& ev) {
    cinux::gui::Event gui_ev{};
    gui_ev.type_     = ev.pressed ? cinux::gui::EventType::KeyDown : cinux::gui::EventType::KeyUp;
    gui_ev.key.ascii = ev.ascii;
    gui_ev.key.scancode = ev.scancode;
    gui_ev.key.pressed  = ev.pressed;
    gui_ev.key.shift    = ev.shift;
    gui_ev.key.ctrl     = ev.ctrl;
    gui_ev.key.alt      = ev.alt;
    cinux::drivers::Mouse::event_queue().enqueue(gui_ev);
    cinux::input::InputEventDevice::instance().push_event(gui_ev);
}
```

（[`gui_init.cpp:29`](kernel/gui/gui_init.cpp#L29)。)注意它把同一个事件塞进**两个队列**:`Mouse::event_queue()` 是老的内核 GUI 队列(现在只有测试还在用),`InputEventDevice::instance().push_event()` 是 `/dev/event0` 的环形缓冲——后者才是 userspace host `read` 的来源。这就是「dual-write」:同一份事件,内核老路径和 userspace 路径各收一份,迁移期内两条路都能跑,迁完可以把老队列拆掉。

鼠标那一侧不用 listener,因为鼠标驱动自己就调 `push_event`。看 [`mouse.cpp`](kernel/drivers/mouse/mouse.cpp#L273):鼠标 IRQ12 的 ISR 在解码完一个数据包后,既 enqueue 进老队列、又 mirror 进 `/dev/event0`。move 分支是这样:

```cpp
// 如果有移动,enqueue 一个 MouseMove 事件
if (ev_dx != 0 || ev_dy != 0) {
    Event ev{};
    ev.type_ = EventType::MouseMove;
    ev.mouse = me;
    g_event_queue_.enqueue(ev);                                  // 老队列(测试用)
    cinux::input::InputEventDevice::instance().push_event(ev);   // /dev/event0(生产用)
}
```

（`move` 分支在 [`mouse.cpp:274`](kernel/drivers/mouse/mouse.cpp#L274) 起,`push_event` 那一行在 [`:281`](kernel/drivers/mouse/mouse.cpp#L281)。)鼠标按键的 down/up 也是同样的 dual-write,每个分支都成对出现([`:289/:292`](kernel/drivers/mouse/mouse.cpp#L289)、[`:298/:301`](kernel/drivers/mouse/mouse.cpp#L298)、…)。

`InputEventDevice` 本身是个 `/dev/event0` 的 InodeOps 适配器,内部一个环形缓冲 + 一把自旋锁。**为什么这里要锁,而老 `EventQueue` 不用?** 因为老队列是 SPSC(单生产者单消费者)——只有鼠标 ISR 推、只有内核 GUI worker 消费。`/dev/event0` 有**两个生产者**(鼠标 IRQ12 + 键盘 listener),所以必须用锁保护的环形缓冲。消费者那一侧,host 进程的 `read`/`poll` 走 syscall 进内核,ops 适配器从环形缓冲里取事件、必要时把 reader 挂起等。

### desktop_launch.cpp:fork+execve 把 host 拉起来

第二个文件,[`desktop_launch.cpp`](kernel/gui/desktop_launch.cpp),全文 74 行,核心是 `launch_userspace()`:

```cpp
void launch_userspace() {
    cinux::lib::kprintf("[INIT] ===== Milestone 035: GUI Desktop (b3b userspace) =====\n");

    // 装 PS/2 鼠标 + 键盘 listener(listener 把每个 KeyEvent mirror 进 /dev/event0)
    cinux::gui::gui_start();

    // fork+execve userspace GUI host。它 open /dev/fb0 + /dev/event0、
    // 建控件树、永远 pump GuiCore。替代旧的内核 gui_worker 线程。
    int child_pid = fork(g_pid_alloc);
    if (child_pid == 0) {
        auto* child        = Scheduler::current();
        child->addr_space  = new cinux::mm::AddressSpace();
        const char* argv[] = {"/cinux_gui_host", "0", nullptr};
        const char* envp[] = {nullptr};
        launch_user_program("/cinux_gui_host", argv, envp);
        Scheduler::exit_current();  // unreachable
    }
    cinux::lib::kprintf("[INIT] userspace GUI host launched (pid=%d)\n", child_pid);
}
```

（[`desktop_launch.cpp:36`](kernel/gui/desktop_launch.cpp#L36)。)这就是「内核拉起桌面」的全部:**先 `gui_start()` 把输入侧装好(否则 host 进程起来读 `/dev/event0` 也读不到东西),再 `fork` 一个新 task、给它一个全新的地址空间、`launch_user_program` 加载 `/cinux_gui_host` 这个 ELF 并跳进 ring3**。`launch_user_program` 是 035 讲过的那个「execve + 建用户栈 + 跳用户态」共享函数(永不返回,失败 `exit_current` 兜底);argv `"0"` 是告诉 host「无限 pump」(看上面 host main 的 `iter` 解析)。fork 完父进程(就是 `kernel_init`)打印一行 pid 就返回了——桌面从此是个独立的 userspace 进程,内核继续往下走自己的 init 流程。

文件里还有个 `handoff_framebuffer_to_gui`（[`desktop_launch.cpp:60`](kernel/gui/desktop_launch.cpp#L60)),它现在的活儿非常薄:**什么 GUI 结构都不建**(host 进程自己建),只是把文本控制台 detach 掉,让例行 kprintf 不再覆盖 framebuffer:

```cpp
void handoff_framebuffer_to_gui(cinux::drivers::Framebuffer& fb, cinux::drivers::PSFFont& font,
                                cinux::drivers::Console& console) {
    (void)fb;
    (void)font;
    cinux::lib::kprintf_set_sink_enabled(cinux::drivers::Console::console_sink_adapter, &console,
                                         false);
}
```

`fb`/`font` 形参都 `(void)` 掉了——它们留在接口上是为了 §14 的接口对称(非 GUI 那边的对应函数签名得对齐),但函数体不用。`fb` 的初始化和 `set_system_framebuffer` 在更早的 `kernel_main` 里就跑过了,所以 `/dev/fb0` 的 mmap 在 host 进程里能正确解析到 VBE framebuffer 的物理内存。`kpanic` 重新启用控制台 sink,所以崩了还是能看到堆栈——这条 detach 是单向的运行期开关,不是配置。

### file gate:GUI 启动 vs 非 GUI 启动

`launch_userspace()` 这个函数有**两份实现**:`kernel/gui/desktop_launch.cpp`(GUI 版,fork+execve `/cinux_gui_host`)和 `kernel/proc/shell_launch.cpp`(非 GUI 版,fork+execve `/bin/sh`)。CMake 按开关选编一份:

```cmake
# §14: shell_launch.cpp is the non-GUI userspace entry (fork + exec /bin/sh).
# GUI builds link kernel/gui/desktop_launch.cpp instead (the gui/ subdirectory is
# added only under if(CINUX_GUI) in kernel/CMakeLists.txt).  Exactly one of the
# two launch_userspace() impls is linked.
if(NOT CINUX_GUI)
    target_sources(big_kernel_common PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/shell_launch.cpp
    )
endif()
```

（[`kernel/proc/CMakeLists.txt:37`](kernel/proc/CMakeLists.txt#L37)。)调用方永远是一句 `cinux::proc::launch_userspace(...)`,零 `#ifdef`——链接器在链接期自动解析到被选中的那份。这叫 **file gate**(文件级开关),它和源码级 `#ifdef` 的区别是:file gate 切的是「编哪个文件」,源码读起来是一条直线;`#ifdef` 切的是「读哪段」,源码读起来要分叉。下一卷会专门讲这套收尾。

## 整条数据路径:从鼠标挪一格到屏幕像素变

把上面所有零件串起来,看一次完整的「鼠标挪一格,屏幕上光标跟着动」走的是哪条路:

1. **鼠标硬件**产生中断,IRQ12 进内核 ISR(`mouse_irq12_handler`)。
2. ISR 解码数据包,算出新光标位置 + button 状态,构造一个 `cinux::gui::Event{type_=MouseMove, mouse={x,y,dx,dy,buttons}}`。
3. ISR 把这个 event **dual-write**:一份进老的 `Mouse::event_queue()`(测试用),一份 `InputEventDevice::instance().push_event()` 进 `/dev/event0` 的环形缓冲（[`mouse.cpp:281`](kernel/drivers/mouse/mouse.cpp#L281))。
4. 与此同时,userspace 的 `/cinux_gui_host` 进程正在它的 `for(;;) core->pump()` 循环里。`pump()` 第 1 步调 `host_poll_event`,后者 `poll(ev_fd, 0)` 探到 `/dev/event0` 有数据,`read` 一个 `kernel_event` 出来,翻译成 `EventHeader{type=kPointer} + PointerPayload{kind=Move, x, y}`。
5. `pump()` 把这个事件交给 `host_dispatch_event`,后者 `wm.process_pointer(p)`——WindowManager 更新光标位置、标光标足迹脏(`invalidate(old + new footprint)`)。
6. `pump()` 第 2 步调 `host_render_frame`:`desktop.render(staging, font, &dirty)` 把控件树 + 新光标画进 core 拥有的 staging 缓冲,报告脏矩形(目前 host 故意报全屏)。
7. `pump()` 第 3 步把脏矩形过一遍 `Region`(去重 / 坍缩)。
8. `pump()` 第 4 步对每个 region 矩形调 `host_flush`,后者把 staging 里对应那块像素 memcpy 进 mmap 的 framebuffer——**屏幕像素变了**。

整条路径里,**内核只做了第 1-3 步**(ISR + push_event),**第 4-8 步全在 userspace**。鼠标解码在内核(因为 PS/2 是中断驱动的硬件协议),光栅化/合成/写 fb 全在用户态。这就是「薄接缝」的含义:内核只负责「把硬件事件变成可读的字节流」,剩下全是用户态进程的事。

## 验证

这一章的验证不靠 QEMU 截图,而是靠两层互锁的证据:

**第一层,core 真的能脱离内核编。** 进 [`third_party/Cinux-GUI/`](third_party/Cinux-GUI/) 子模块,跑它自己的 standalone ctest:`cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j$(nproc) && ctest --test-dir build --output-on-failure`。绿就证明 core 在没有任何内核参与的情况下能编能跑,host-neutral 不是嘴上说的。host 单测想更严,push 前自验开 ASAN:`-DCMAKE_CXX_FLAGS="-fsanitize=address" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address"`(本地默认 ctest 不开 ASAN 会漏判)。

**第二层,host 进程能起来 + 屏幕真有像素。** 跑构建脚本 [`tools/musl/build-cinux-gui-host.sh`](tools/musl/build-cinux-gui-host.sh) 出静态 musl ELF,塞进 initramfs,内核起来后看串口应该有这两行(都来自源码 kprintf):

```
[INIT] ===== Milestone 035: GUI Desktop (b3b userspace) =====
[INIT] userspace GUI host launched (pid=...)
```

host 进程冒烟自检那个「跑 N 圈读中心像素非零」的逻辑([`main.cpp:578`](user/cinux_gui_host/main.cpp#L578))是非零返回 5,跑通了就是 0。QEMU 下能看见桌面 + Hello 窗口 + Shell/Calculator 图标,点 Shell 弹出终端窗口、能输命令——这一连串就是上面「整条数据路径」的可见版本。具体动手步骤、坑、断点该看哪里,见配套 lab。

## 这章没做的

像 051 那样,把没做完的事说清楚,免得读者把骨架当成成品。

**脏矩形优化实际没生效(但原因不是「core 没修」,是「host 的全屏 flush 没拆」)。** 上面讲 `host_render_frame` 时说过:host 现在每帧都报一个全屏脏矩形,把 `pump()` 的 idle 跳过和 Region 去重实际短路掉了。这件事的来龙去脉再说一遍,因为它容易让读者误会:**当年** core 的 `WindowManager::remove_window` 关窗不标脏,host 只能每帧全屏 flush 兜底;**现在** core 那个 bug 早已修掉(`remove_window` 在 [`window_manager.cpp:58`](third_party/Cinux-GUI/core/widget/window_manager.cpp#L58) 已经 `invalidate(stale)`,`add_window`/`add_icon`/光标移动/Z-order 变化也都各自 invalidate),所以 host 的全屏 flush 在原理上已经不必要。**没做的是把 host 改回报真脏区**——把 `host_render_frame` 里那段「强制 `frame->count = 1` 全屏」拆掉、改成把 `desktop.render` 算出的 `dirty` Region 原样喂回 `frame->rects`,然后实跑验证关窗路径真不留残影。这是个范围明确的收尾活,本章没做。「空闲不空转」「只推变化像素」这两个设计目标因此目前没真正落地,但卡在 host 这一侧,不在 core。

**`/dev/event0` 还是 dual-write,老队列没拆。** 鼠标 ISR 和键盘 listener 现在每产生一个事件就同时塞进老 `Mouse::event_queue()` 和 `/dev/event0`。老队列只有 `test_mouse_event` 这类内核测试还在用,生产路径已经走 `/dev/event0` 了。把老队列彻底拆掉、ISR 只 push `/dev/event0`,是后续清理的事——目前 dual-write 是为了迁移期内两条路都能跑、测试不破。

**软件光栅化(`swraster`)是 core 的一等公民,但 host 没全用它。** core 里 `swraster.cpp` 是纯整数的软件光栅化器(Q8.8 定点,不引浮点),控件树的 `render` 路径确实走它。但 host 进程里有些绘制(比如 `host_flush` 那一层的 memcpy)还是手写循环,没全归到 swraster。这不算 bug,只是「光栅化统一收口」没收完。

**host 进程的 stdout 重定向是个 hack。** host main 开头有一段 `open("/dev/console", O_WRONLY); dup2(cfd, 1); dup2(cfd, 2);`（[`main.cpp:477`](user/cinux_gui_host/main.cpp#L477))——因为 fork+execve 出来的 host 继承的 fd 表不一定把 stdout 接到串口控制台,host 想打日志(`host_log` 用 `printf`)就得自己重定向。注释自己也写了「redirect stdout/stderr to /dev/console so host_log reaches the serial log」。这是个实用主义的补丁,不是架构问题,但读者看到别以为这是「正经的 stdio 接线」。

**Calculator 图标是个 stub。** 桌面上的 Shell 图标点击会 spawn `/bin/sh`(走一套 `open("/dev/ptmx")` + `ioctl(TIOCGPTN)` + `open("/dev/pts/N")` + `TIOCSCTTY` + `fork` + `execve` + `TerminalWidget` + PTY drain 的完整路径,在 [`shell_activate`](user/cinux_gui_host/main.cpp#L387) 里——这套 `/dev/ptmx` + `TIOCGPTN` + `/dev/pts/N` + `TIOCSCTTY` 正是 066 立的 PTY ABI,host 这里只是它的一个用户),但 Calculator 图标的 `calc_activate` 是个空函数（[`main.cpp:467`](user/cinux_gui_host/main.cpp#L467))——点它什么都不会发生。注释里就一行 `// stub,calculator 还没接`。这是个留给后续的占位。

**host 没有 CPU 让步,编译时会卡。** `host_poll_event` 那段注释提了:host 用 `poll(ev_fd, 0)` 非阻塞,所以它在 `for(;;) core->pump()` 里**忙等**——除了 `poll` 没拿到事件时那一小段,其余时间 CPU 是满的。更糟的是:当 shell 里跑 `gcc` 这种重活,因为 NVMe 驱动的同步 poll 不能 yield,host 进程连同整个桌面都会卡住。这不是本章的设计缺陷,而是 v1.0.0 一个已知问题,正确修法是异步 IO(v1.1+)。本章把 host 立起来,但没解决这个让步问题——读者点 Shell 跑 gcc 看见桌面冻住别以为是自己配错了。

## 小结

这一章干了一件「搬家」:把 GUI 从内核里整个拔出来,搬成三层——**host-neutral core**(几何 + 事件泵 + 光栅化,不 include 任何内核头)、**Host ABI 表**(core 和宿主之间唯一的硬接缝,一张函数指针表,谁填表 core 就跑在谁上面)、**userspace host 进程**(普通的 ring3 ELF,`open`/`mmap`/`read`/`poll` 全是 syscall)。内核这边只剩 `gui_init.cpp`(ISR 里 dual-write 事件到 `/dev/event0`)和 `desktop_launch.cpp`(fork+execve `/cinux_gui_host`)两个文件,旧的 `host_cinux.cpp` 已删。

三件值得记住的硬规矩:**flush 显示模型**(core 拥 staging 缓冲,host 只被邀请来画 + 报脏帧);**Region 永不欠覆盖**(容量溢出坍缩成包围盒,多推几个像素是性能损失,漏推一个变了的像素是视觉 bug);**file gate 切文件不切源码**(`launch_userspace()` 两份实现,CMake 选编一份,调用处零 `#ifdef`)。还有一条已经认下的债:host 的全屏 flush workaround 是补一个**早已修掉的** core bug 留下的,把它拆回报真脏区是个收尾活——本章搭的是骨架,不是成品。

下一章 055 接 xHCI USB——那一步落地后,鼠标输入才有真 USB 可走(现在只有 PS/2);087 讲 `/dev/fb0` + `/dev/event0` 这两个设备 fd 的设备模型细节,本章把它们当黑盒用;066 讲 PTY,host 那套 `open("/dev/ptmx")` + `TIOCGPTN` + `/dev/pts/N` 的 shell 启动路径就建立在它上面。
