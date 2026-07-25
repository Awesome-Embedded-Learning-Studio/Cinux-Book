---
title: 02 · Host ABI 表 + GuiCore pump + Region
---

# Host ABI 表 + GuiCore pump + Region

## Host ABI 表:core 和宿主之间唯一的接缝

这道边界两边靠什么通话?靠一张纯数据的函数指针表。它在 [`core/host.hpp`](../../../third_party/Cinux-GUI/core/host.hpp) 里,叫 `Host`:

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

（[`host.hpp:56`](../../../third_party/Cinux-GUI/core/host.hpp#L56) 起。）这张表里全是**函数指针**——core 永远不直接调 framebuffer 或鼠标队列,它调的是 `host->core.flush(...)` 这种间接调用。这就引出这一整弧最关键的一句话:**谁填这张表,core 就跑在谁上面**。

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

（[`host.hpp:42`](../../../third_party/Cinux-GUI/core/host.hpp#L42)。)注意 `pixels`/`stride`/`width`/`height`/`format` 全是 **core 预填好的**——staging 缓冲的归属权在 core 这边,host 只是被邀请进来「在这块缓冲上画」。host 写回的只有 `rects[]` 和 `count`:这一帧它弄脏了哪几块。**core 拥 staging,host 只收脏帧**——这是 flush 显示模型的核心,它让 core 不用关心 host 怎么画、画给谁看。

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

（[`gui_core.cpp:44`](../../../third_party/Cinux-GUI/core/gui_core.cpp#L44),idle 跳过在 [`:79`](../../../third_party/Cinux-GUI/core/gui_core.cpp#L79)。) `pump()` 自己**做不了任何 GUI 的事**——它没有窗口管理器、没有渲染器、不知道屏幕在哪。它只是个协议骨架:排空输入、把 staging 缓冲递给 host 让它画、把 host 报告的脏区收拢、推走。整个函数对宿主是什么类型一无所知,这就是「不感知自己跑在内核态还是用户态」的实现机制。

注意第 2 步尾部那个 idle 跳过:`render_frame` 返回后立刻判 `frame.count == 0`(这一帧什么都没变),`pump()` 直接 `return`,**连一个像素都不 flush**。这是后面「空闲不空转」的关键——前提是 host 那一侧能准确报告脏区。Cinux 的 userspace host 现在还没做到这点,下面讲。

## Region:脏矩形代数,永不欠覆盖

第 3 步把 host 报告的脏矩形收拢,用的是 core 自己的 `Region`——一个**定容**的矩形集合。先说为什么需要它,再说它的硬规矩。

一帧里「变了」的往往不止一块:鼠标挪一格、终端吐一个字符。如果每帧都把整屏(1024×768,约 3 MB)从 staging 整块搬到帧缓冲,绝大多数搬运是浪费——帧缓冲走的是 MMIO 总线时尤其浪费。脏矩形的思路:渲染照常全帧画进 staging(保证内容恒新),但只把**这一帧变了的那几块**矩形覆盖的像素推给显示端。

Cinux 给这套几何立了一条硬规矩,写在 [`region.hpp`](../../../third_party/Cinux-GUI/core/region.hpp#L10) 顶部:**宁可多推,绝不漏推**。多推几个没变的像素,是看不见的性能损失;漏推一个变了的像素,是屏幕上残留旧内容的视觉 bug。这条规矩贯穿下面所有的设计。

脏矩形运算建立在**半开区间**的 `Rect` 上:`[x0, x1) × [y0, y1)`,左/上含、右/下不含。一个宽 4 的矩形写成 `{0,0,4,4}`,覆盖的是第 0、1、2、3 列(不是 4 列)。这样两个相邻矩形 `{0,0,4,4}` 和 `{4,0,8,4}` 无缝拼接——共享的那条边 `x=4` 只属于右边那个,不重叠也不漏缝。退化(空)矩形的判据也就干净了:`x0 >= x1 || y0 >= y1`（[`region.hpp:47`](../../../third_party/Cinux-GUI/core/region.hpp#L47))。

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

（[`region.hpp:113`](../../../third_party/Cinux-GUI/core/region.hpp#L113) 起,容量常量 `kMaxRects = 32` 在 [`:116`](../../../third_party/Cinux-GUI/core/region.hpp#L116)。)定容、`add()` 不分配堆——这是为了让最常见的「一帧累积脏区」这条热路径能在任何上下文里跑。那塞到第 33 个怎么办?它不报错也不扩容,而是把当前所有矩形 + 新矩形的**包围盒**算出来,整个 `Region` 坍缩成这一个矩形:

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

（[`region.cpp:51`](../../../third_party/Cinux-GUI/core/region.cpp#L51)。)这就是「宁可多推绝不漏推」的落地:包围盒盖住了所有原本的矩形(可能也盖了些本来没变的像素),但绝不会漏掉任何一个变了的像素。对脏区来说,过覆盖是性能问题,欠覆盖才是正确性问题,所以一律取前者。

> **一个真实的坑,别照搬「不分配堆」当不变量。** `add()` 确实不分配堆(热路径,任何上下文能跑),但同类的 `Region::subtract()` 不一样——它会 `new Rect[kMaxRects*4]` 临时缓冲(`region.cpp:92`)。原因是减法能把一个矩形切成最多 4 块,32 个矩形一减就 128 个,在内核栈上撑不住。注释专门标了这条「heap-allocated: Rect[kMaxRects*4] (128 rects ~= 2 KB) on the stack busts a tight kernel stack」。要说清楚的是:`subtract` 目前只是 `Region` 暴露给将来 occlusion / 切割路径的 API,**core 现在的渲染路径还没调它**(`pump()` 第 3 步只调 `add`,widget/compositor 里也没有 subtract 调用点)——但它的实现确实上堆,所以「`Region` 整个类零分配」这个直觉是错的。记住 add 零分配、subtract 上堆,就够了。

