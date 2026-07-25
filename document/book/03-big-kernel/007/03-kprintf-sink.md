---
title: 03 · kprintf 多路 sink 与装配顺序
---

# kprintf 多路 sink 与装配顺序

## kprintf 多路 sink:引擎不动,只换分派

现在到了这一章的核心:kprintf 怎么从「只走串口」变成「串口 + 屏幕」。

先看它升级后的对外接口([kprintf.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/lib/kprintf.hpp)):

```cpp
using OutputSink = void(*)(char c, void* ctx);   // sink 的类型: 一个函数指针 + 一个上下文
static constexpr uint32_t KPRINTF_MAX_SINKS = 8; // 最多 8 路后端

void kprintf_register_sink(OutputSink fn, void* ctx);  // 注册一路后端
```

`OutputSink` 是个函数指针,吃一个字符、吃一个 `void*` 上下文。`void* ctx` 是关键——它让同一个 sink 函数能服务不同的实例(比如两个 Console),注册时把实例指针塞进去,回调时再 `static_cast` 回来。这正是上面 `console_sink_adapter` 拿 `ctx` 当 `Console*` 用的道理。

内部是一张 sink 表([kprintf.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/lib/kprintf.cpp)):

```cpp
struct Sink { OutputSink fn; void* ctx; bool enabled; };
static Sink     g_sinks[KPRINTF_MAX_SINKS] = {};
static uint32_t g_sink_count = 0;

void kprintf_register_sink(OutputSink fn, void* ctx) {
    if (fn == nullptr) return;
    for (uint32_t i = 0; i < g_sink_count; i++)        // 先回收已禁用的槽
        if (!g_sinks[i].enabled) { g_sinks[i] = {fn, ctx, true}; return; }
    if (g_sink_count < KPRINTF_MAX_SINKS)              // 再追加新槽
        g_sinks[g_sink_count++] = {fn, ctx, true};
}
```

注册逻辑很简单:先看有没有「已注册但被禁用」的空槽可以复用,没有就在末尾追加,满了就丢弃(最多 8 路,够用了)。`enabled` 字段此刻没在别处置 false,是为以后「临时关掉某路输出」留的口子。

真正体现了「引擎不动」的,是 `kprintf` 本体:

```cpp
void kprintf(const char* fmt, ...) {
    va_list args; va_start(args, fmt);
    vkprintf_impl([&](char c) {                       // ← 还是那个引擎, 签名一字未改
        for (uint32_t i = 0; i < g_sink_count; i++)    //   只是把回调体换成了 fan-out
            if (g_sinks[i].enabled)
                g_sinks[i].fn(c, g_sinks[i].ctx);
    }, fmt, args);
    va_end(args);
}
```

和 012 的版本逐字对比一下,差别只在那个 lambda:012 是 `[&](char c){ g_serial.putc(c); }`(硬编码喂串口),现在是 `[&](char c){ 遍历 g_sinks 逐个喂 }`。而 `vkprintf_impl(引擎, fmt, args)` 这一行的形状、引擎内部的所有格式化逻辑(宽度、对齐、`%p`、负数零补那些),一个字都没动。`kvprintf`、`kpanic` 是同样的改法——三个函数,各自把喂串口的 lambda 换成遍历 sink 表的 lambda。

这就是 012 那层抽象的回报。当时我们把「格式化」和「输出」用回调切开,看起来像过度设计——只有一个串口后端时,直接调 `serial.putc` 不是更简单吗?但只要后端可能变多,这层切开就值了:现在加屏幕这一路,我们没碰格式化引擎(那是最容易引入 bug 的地方),只动了输出分派。如果以后还想加一路「打到 QEMU debugcon」或者「写到一个环形缓冲区给 dmesg 用」,照样只是再 `kprintf_register_sink` 一个新 adapter,引擎依旧不动。

初始化时,串口作为第一路 sink 注册:

```cpp
static Serial g_serial(SERIAL_COM1);            // big kernel 的单例串口
void serial_sink_adapter(char c, void*) { g_serial.putc(c); }

void kprintf_init() {
    g_serial.init();
    kprintf_register_sink(serial_sink_adapter, nullptr);   // sink[0] = 串口
}
```

所以 `kprintf_init` 之后,kprintf 已经能走串口了(和 012 行为一致)。屏幕这一路,要等 main 里 Console 建好之后再注册——这就引出下一节的装配顺序。

## 装配顺序为什么不能乱

把 fb、font、console、kprintf 这些东西在 `kernel_main` 里拼起来,顺序很重要。看 [main.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/main.cpp) 这一段(省略前面的 GDT/IDT/PIC/PIT):

```cpp
// 串口先就位 → kprintf 能用了(串口这一路)
cinux::lib::kprintf_init();

// ... GDT / IDT / PIC / irq_init / PIT / int $3 ...

// 1. framebuffer: 映射显存, 能画像素了
auto* boot_info = reinterpret_cast<const BootInfo*>(BOOT_INFO_PHYS);
Framebuffer fb;
fb.init(*boot_info);

// 2. 字体: 解析嵌入的 PSF2
PSFFont font;
font.init();

// 3. 控制台: 把 fb + font 捏成字符网格, 并清屏
Console console;
console.init(fb, font, 0x00FFFFFF, 0x00000000);

// 4. 把控制台注册成 kprintf 的第二路 sink → 双输出生效
cinux::lib::kprintf_register_sink(Console::console_sink_adapter, &console);
cinux::lib::kprintf("[BIG] Console initialised -- dual output active.\n");
```

这个顺序背后是严格的依赖链,任何一步提前都会出问题:

- `kprintf_init` 必须最靠前——后面所有步骤的诊断都靠它,它不通,后面崩了你都不知道崩在哪。
- `fb.init` 必须在 `font.init` 和 `console.init` 之前——font 渲染要往 fb 里写像素,console 初始化时要算 `cols/rows` 还要 `clear`(往 fb 写)。fb 没就位,这俩都会写到没映射的地址。
- `console.init` 必须在 `kprintf_register_sink(console)` 之前——你注册给 kprintf 的 sink,回调里会 `console->putc()`,console 没初始化(`fb_` 是 nullptr)就什么也不画,屏幕照样黑。
- 而 `kprintf_register_sink` 本身又必须在 `kprintf_init` 之后——因为 sink 表的初始化依赖 kprintf 模块已就绪。

所以正确的链是:`kprintf_init(串口)` → `fb.init` → `font.init` → `console.init` → `register_sink(console)`。main 的头注释把这条链列得很清楚,从 Step 1 到 Step 11,每一步都标了「depends on 什么」。这种 init 顺序在内核里到处都是,养成「先想清楚依赖、再排顺序」的习惯,能省下大量「为什么屏幕是黑的」式的排查。

注册完 console sink 之后,那句 `[BIG] Console initialised -- dual output active.` 就会**同时**出现在串口和屏幕上——因为 kprintf 的 fan-out 已经把每个字符发给两路 sink 了。从这一刻起,内核说的每一句话都有两个出口。

## 顺手把 drivers 目录理顺

这一章还有一笔「家务事」:把驱动目录理一理。在这之前,`pit.cpp`、`serial.cpp` 都直接堆在 `kernel/drivers/` 根下:

```text
改前:                              改后:
kernel/drivers/                    kernel/drivers/
├── pit.cpp                        ├── pit/
├── pit.hpp                        │   ├── pit.cpp
├── serial.cpp                     │   └── pit.hpp
└── serial.hpp                     └── serial/
                                       ├── serial.cpp
                                       └── serial.hpp
```

这其实就是 012 那一章欠下的账——还记得吗,`012_driver_serial` 这个 tag 名里带着 serial,但它其实没干 serial 的活(干的是 kprintf 和 SSE),我们当时说「serial 的目录化在 013」。这一章把它还了:`serial` 和 `pit` 各自挪进自己的子目录,跟新加的 `video/` 子目录保持一致(`video/` 下放着 framebuffer、font、console)。include 路径也跟着从 `"kernel/drivers/serial.hpp"` 改成 `"kernel/drivers/serial/serial.hpp"`。

这是个纯组织性改动,没有功能变化,但值得做:驱动一多,全堆在 `drivers/` 根下很快就会乱成一锅粥。按设备类型分子目录(pit/、serial/、video/、后面还会有 keyboard/),是操作系统代码库里很自然的组织方式。CMakeLists 里对应的源文件路径也跟着更新了一行。
