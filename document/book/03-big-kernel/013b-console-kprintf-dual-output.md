---
title: 013b · 文本控制台与 kprintf 双输出
---

# 013b · 文本控制台与 kprintf 双输出:012 那个回调终于接上了屏幕

> 上一章(013)我们把屏幕这块「舞台」搭好了:显存能映射、像素能画、字能显。但留了个尾巴——这套画字能力还没人用,kprintf 依旧只走串口。这一章收尾:在像素之上盖一层文本控制台 `Console`,管好光标、换行、滚动;然后回头兑现 012 埋下的那个承诺——让 kprintf 的格式化引擎接上屏幕这第二个输出后端。做完这一步,内核每一句诊断才会同时出现在串口和屏幕上,你终于不用为了看它说什么而专门开个串口窗口了。

## 这一章我们要点亮什么

两件事,一软一硬地接上 013。

第一件,屏幕上不再只是一堆散乱的像素,而是一个**会换行、会滚动**的文本终端。你往里写 `"Hello\n"`,它就在第一行画出来、光标移到第二行;写满了,整个画面往上滚一行,底下腾出新的一行继续写——就像你最熟悉的那个终端一样。这一切由 `Console` 这个类负责,它把 013 的 framebuffer + 字体捏成一个「字符网格」。

第二件,是这一章真正的重头戏:kprintf 接上屏幕。012 那时我们把 kprintf 的格式化逻辑抽成了一个只认回调的引擎 `vkprintf_impl`,当时只喂了串口一个 lambda,并预告「以后还会多一路屏幕」。这一章我们把「输出后端」从一个升级成**一组**——一个最多 8 路的 sink 表,串口和屏幕各占一路,格式化引擎一行不改,每次产出的字符同时分发给所有已注册的后端。这就是 012 那层回调解耦真正兑现的时刻。

## 为什么现在需要它

先回答一个直白的问题:013 已经能画字了,为什么还要单独搞个 Console,不能直接 `kprintf` 一个字符一个字符往屏幕上画吗?

能,但你会立刻撞上「光标在哪、写到屏幕边缘怎么办、写满了怎么办」这一连串问题。像素坐标是 `(x, y)`,可人写日志是按「行和列」思考的——`"Hello"` 该从第几行第几列开始画?画完光标停哪?这些事 framebuffer 和字体都不管,它们只认像素。Console 就是填这层缝隙的:它在像素之上抽象出一个 `行 × 列` 的网格,记住光标的 `(row, col)`,每写一个字符就把光标往前推、到行尾就换行、写满就滚动。有了它,kprintf 才能用最朴素的方式「逐字符写」,不用操心位置。

至于 kprintf 接屏幕,那就更不是「锦上添花」了。到这一步为止,内核所有诊断信息都只走串口——这意味着每次想看内核状态,你都得开个串口终端窗口、连上 QEMU 的串口。屏幕一直黑着,白白浪费。把 kprintf 接上屏幕,诊断信息就有了第二条、也是更直观的一条出口:开机就能看见 `[BIG] GDT loaded.` 一行行刷出来,不用任何额外工具。对一个正在成长的内核,这种「看得见」的反馈极其重要。

而且这一步几乎不花成本——因为 012 把路铺好了。回头看 012 的 kprintf:格式化引擎 `vkprintf_impl` 完全硬件无关,只认一个「输出单字符」的回调。当时它的回调是「喂串口」。现在我们要做的,仅仅是把「喂串口」换成「喂一组后端」,引擎本身一个字都不用动。这就是当初那层抽象的红利:加一个输出后端,不用碰最容易出错的格式化逻辑。

## 设计图

Console 是一个架在 framebuffer + 字体之上的状态机,核心就两个状态:`row`(当前第几行)和 `col`(当前第几列)。每写一个字符,根据它是控制符还是可打印字符,走不同分支:

```text
   Console::putc(c)
        │
        ├─ c == '\n'  →  new_line(): col=0; 已到最后一行? scroll() : row++
        ├─ c == '\r'  →  col = 0
        ├─ c == '\b'  →  回退: col>0 则 col--; 否则换到上一行末尾
        └─ 可打印字符
                ├─ col 已到右边界? → 先 new_line() 换行
                ├─ font.render_char(fb, c, col*字宽, row*字高, fg, bg)
                └─ col++
```

kprintf 那边,从 012 的「单回调喂串口」升级成「sink 表 fan-out」:

```text
   012 (单后端):                        013 (多后端):
   kprintf(fmt,...)                     kprintf(fmt,...)
     └ vkprintf_impl(                   └ vkprintf_impl(
          [&](c){ serial.putc(c); },            [&](c){ 遍历 g_sinks[]:
          fmt, args)                                for 每个 enabled sink:
        )                                              sink.fn(c, sink.ctx); },
                                                fmt, args)
                                            │
                                  g_sinks[0] = serial_sink_adapter   (串口)
                                  g_sinks[1] = console_sink_adapter   (屏幕)  ← 本章注册
                                  ... 最多 8 路
```

左半边是 012 的样子:一个 lambda,硬编码喂串口。右半边是这一章:同一个 lambda,改成遍历一张 sink 表,把字符扇出给所有后端。格式化引擎 `vkprintf_impl` 那个框,左右一模一样——这就是「引擎不动」的含义。

## 代码路线

### Console:在像素上盖一层文本网格

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

### kprintf 多路 sink:引擎不动,只换分派

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

### 装配顺序为什么不能乱

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

### 顺手把 drivers 目录理顺

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

## 调试现场

这一章没有 notes 文件,但有两个真实的、装配时高发的坑,值得点出来。

**第一个,「屏幕是黑的但串口有输出」。** 这是最常见的症状,根因几乎总是上面说的装配顺序里某一环断了。排查思路是沿着依赖链倒着查:串口有输出,说明 `kprintf_init` 成功了、串口 sink 在工作;屏幕黑,说明 console sink 没生效。那就查——`console.init` 调了吗?它之前 `fb.init` 和 `font.init` 调了吗?`kprintf_register_sink(console)` 调了吗?最阴的情况是:`register_sink` 调了,但 console 的 `fb_` 是 nullptr(因为 fb 还没 init),于是 `putc` 里第一行 `if (fb_ == nullptr) return;` 直接返回,字符被静默吞掉。`putc` 开头那个 nullptr 检查是个保护,但也意味着「配置错了不会崩,只会安静地不显示」——遇到屏幕黑,别只盯着 console 代码看,先确认它依赖的 fb、font 真的初始化了。

**第二个,「同一个字符在串口和屏幕上不一致」或者「屏幕上少了字符」。** 既然是 fan-out,正常情况下两路应该一字不差。如果屏幕少了字符,多半是 console 的状态机在某个控制符上和串口行为不一致——比如串口对某个字节照单全收,而 console 的 `putc` 把它当成了需要特殊处理的字符,或者 `col_ >= cols_` 的自动换行比预期早触发,把一个字符挤掉了。这种问题用对照法最直接:让内核打一段包含换行、长行、特殊字符的固定文本,串口和屏幕逐字比,第一个不一致的地方就是 bug 现场。

还有一个值得意识到的点:fan-out 意味着**每个字符都被处理两遍**(串口一遍、屏幕一遍)。屏幕那路还要走字体渲染、逐像素 `put_pixel`,比串口慢得多。所以一段很长的 kprintf 输出,瓶颈永远在屏幕渲染这一路。此刻这不构成问题(诊断输出量不大),但心里有这个数,以后如果发现内核启动变慢、又恰好在打一大段日志,就知道去哪找。

## 验证

这一章的验证,核心就是「双输出真的生效了」。

最直接的现象验证:把内核 `make run`(或对应 CMake target)起来,你会看到 012 就有的那段 kprintf 格式回归输出,**现在不仅在串口、也在屏幕上**刷出来。关键看那句:

```text
[BIG] Console initialised -- dual output active.
```

它出现在屏幕上,就证明 console sink 注册成功、fan-out 在工作。从这句之后,所有 `[BIG]` 开头的诊断都是串口和屏幕同步的。

机内测里,[test_video.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_video.cpp) 也覆盖了 console,验证它能正确地 `putc`、能在写满后触发滚动。和 013 的 fb/font 测试一起跑:

```bash
cmake --build build --target run-big-kernel-test
```

console 的纯逻辑(光标移动、换行、回卷、控制符处理),有 host 单测 [test_console.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_console.cpp) 镜像测,不依赖 QEMU:

```bash
ctest --test-dir build -R console --output-on-failure
```

这些测的是 `putc` 状态机的算术:写 N 个字符后 `(row, col)` 在哪、`\n` 之后在哪、写满一行触发换行没触发滚动、写满整屏才触发滚动。它们绿的,console 的行为就是对的。

## 下一站

到这里,内核第一次有了「脸」:它能在屏幕上画字,kprintf 的每一句话都能直接在屏幕上看见,不用再开串口窗口。诊断通道从一根线(串口)变成了两根线(串口 + 屏幕),双输出稳定工作。

但你会发现一个明显的缺口:这台机器**只会说,不会听**。屏幕能显示,可键盘敲进去的字符,内核一个都收不到——IRQ1 上挂的还是那个只发 EOI 就把字符丢掉的 default handler,和 011 结束时一模一样。我们装了一整套中断体系,却还听不见键盘。

打破这个局限,就是下一站的事。键盘要接上真 handler,得搞定扫描码、IRQ1 的真处理、还有怎么把收到的字符送回屏幕(你会发现,刚搭好的 console 又要派上用场了)。不过那是下一章的故事,这里我们先享受一下「内核终于能在屏幕上说话」这个里程碑。

---

### 参考

- 012 章 · [kprintf 重构与引导期 SSE 初始化](012-kprintf-sse.md):`vkprintf_impl` 回调式格式化引擎的来历。本章的多路 sink 正是建立在那层回调解耦之上——引擎未改,只换了输出分派。
- OSDev — [Text UI / Text Mode Console](https://wiki.osdev.org/Text_UI):在帧缓冲上手搓文本控制台时,光标跟踪、自动换行、滚动这几件事的常见做法。本章 Console 的状态机与此一致(只是画在图形帧缓冲上,而非 VGA 文本模式)。
- 本 tag 源码:[console.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/video/console.hpp) / [console.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/video/console.cpp)、[kprintf.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/lib/kprintf.hpp) / [kprintf.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/lib/kprintf.cpp)(多路 sink)、[main.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/main.cpp)(装配序);驱动重组见 [pit](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/pit/)、[serial](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/serial/);测试 [test_console.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_console.cpp)、[test_video.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_video.cpp)。
