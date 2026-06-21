---
title: Lab 013 · 让内核在屏幕上说话:帧缓冲 + 字体 + 控制台
---

# Lab 013 · 让内核在屏幕上说话:帧缓冲 + 字体 + 控制台

> 配套章节:[013 · 把字写到屏幕上](../../book/03-big-kernel/013-vga-framebuffer.md)、[013b · 文本控制台与 kprintf 双输出](../../book/03-big-kernel/013b-console-kprintf-dual-output.md)。这一关给你目标和约束,不贴答案——从「显存映射不进来」一路做到「kprintf 同时在串口和屏幕上说话」。

## 实验目标

一条主线:让内核的输出第一次出现在屏幕上。具体拆成几个能独立验证的子目标:

1. 显存能访问:bootloader 给你的那块物理帧缓冲,你能映射进内核虚拟地址、写一个像素屏幕就亮。
2. 字体能画:内置一个点阵字体,能把任意一个字符画成屏幕上的像素块。
3. 有文本网格:在像素之上盖一层「行 × 列」的控制台,会换行、会滚动,光标走得对。
4. kprintf 双输出:kprintf 每产出一个字符,同时进串口和屏幕,不用改格式化引擎。

做完这四条,内核就有了「脸」:开机就能在屏幕上看见诊断信息刷出来。

## 前置条件

你得先过 Lab 012:kprintf 的格式化引擎已经抽成回调式、`-O2`(Release)内核起得来。这一关要在 012 那个回调引擎上「加第二个输出后端」,引擎本身不动——如果 012 的引擎没抽干净、还和串口耦合在一起,这关第一步就会卡。

另外,这一关依赖 bootloader 已经切到图形模式、把帧缓冲情报塞进了 `BootInfo`(物理 0x7000)。VBE 模式切换那一套(实模式 `INT 0x10`)不是这关的活,假定它已经就位、`BootInfo` 里的 `fb_addr/fb_width/fb_height/fb_pitch/fb_bpp` 都有效。

## 任务分解

**第一步:把帧缓冲映射进来。** 这是最容易翻车的一步。`fb_addr` 是物理地址,内核跑在虚拟地址空间,直接解引用会 page fault。你要写一个最小的页表助手,在 bootloader 留下的页表里补条目,把 `[fb_addr, fb_addr+size)` 这段物理内存映射成「虚拟地址 == 物理地址」(恒等映射),这样后面才能拿 `fb_addr` 当虚拟地址用。

提示几个要点:
- 你得知道 bootloader 把页目录(PD)、页目录指针表(PDPT)放在哪个虚拟地址,才能摸进去改表项。这是个写死的约定,脆弱但此刻够用。
- 映射用大页,别用 4KB 页(帧缓冲动辄几 MB,4KB 页条目太多)。物理地址 <1GB 的部分用 2MB 大页(PD 表项,记得 PS 位置 1),≥1GB 的部分用 1GB 大页(PDPT 表项,且得先 `CPUID` 探测 CPU 支不支持 1GB 页)。
- 每补一条 2MB 页表项,记得作废那条地址的 TLB;1GB 页改完,整体刷新 CR3。
- 别覆盖已有的表项(只补空位),免得破坏 bootloader 已经建好的映射。

**第二步:写 framebuffer 驱动。** 映射好了之后,帧缓冲就是个 `uint32_t` 数组。实现 `init(BootInfo)`(调你第一步的映射、存基地址和尺寸)、`put_pixel(x,y,argb)`、`get_pixel`、`fill_rect`、`clear`。关键就一个下标公式:第 `y` 行第 `x` 个像素在 `addr_[y * (pitch/4) + x]`——用 `pitch` 不是 `width`,除以 4 是因为 `pitch` 是字节数而下标按 32 位单元计。再实现一个 `scroll_up`,把整块显存当字节数组往上搬 N 行、清底部空带(这一步后面控制台滚动要用)。

**第三步:内置字体并画字。** 字体用 PSF2 格式。你可以用脚本生成一个 8×16、256 字形的 PSF2 文件(经典 IBM PC CP437 点阵),然后用汇编 `.incbin` 把这个二进制嵌进内核 `.rodata`、导出起止符号。C++ 侧解析 PSF2 header(先校验魔数 `0x864AB572`),拿到字形数据指针和宽高。画字时逐行取 1 字节、逐位判断:`(bits >> (7-col)) & 1` 为 1 画前景、为 0 画背景。注意这套逐字节取位**只对宽度 ≤ 8 的字体成立**,边界要心里有数。

**第四步:盖一层文本控制台。** 在 framebuffer + 字体之上实现 `Console`,状态就是光标 `(row, col)` 加屏幕能容下的 `(rows, cols)`。`init` 时 `cols = fb.width()/font.width()`、`rows = fb.height()/font.height()`,然后清屏。`putc` 是个状态机:`\n` 换行(到最后一行就滚动)、`\r` 回行首、`\b` 退格(行首则退到上一行末)、可打印字符画出来并推进光标(到右边界先换行)。滚动直接委托给第二步的 `fb.scroll_up`,传一个字高进去。

**第五步:接上 kprintf 双输出。** 回头看 012 的 kprintf:格式化引擎只认一个「输出单字符」的回调,当时回调是喂串口。现在把它升级成「一组回调」:维护一张最多 8 路的 sink 表,每项是「函数指针 + void* 上下文」。`kprintf` 每产出一个字符,遍历整张表逐个分发。格式化引擎**一行不改**——只把那个回调体从「喂串口」换成「遍历 sink 表」。然后 `kprintf_init` 注册串口为第一路,main 里 console 建好后注册 console 为第二路。

## 接口约束

你要实现出来的东西,对外长这样(只给职责和签名,不给实现):

- `arch::map_mmio(uint64_t phys, uint64_t size)`:把一段物理内存恒等映射进内核虚拟地址,用大页。最小助手,不做权限/并发管理。
- `Framebuffer`:`init(const BootInfo&)`、`put_pixel(x,y,argb)`、`get_pixel(x,y)`、`fill_rect(x,y,w,h,argb)`、`scroll_up(lines, line_height, bg)`、`clear(argb=0)`,以及 `width()/height()/pitch()` 访问器。像素格式 32bpp `0x00RRGGBB`。
- `PSFFont`:`init()`(解析嵌入的 PSF2)、`render_char(fb, c, x, y, fg, bg)`、`width()/height()`。
- `Console`:`init(fb, font, fg, bg)`、`putc(char)`、`clear()`、`set_color(fg,bg)`,以及一个静态 sink 适配 `console_sink_adapter(char, void* ctx)`。
- kprintf 侧:`using OutputSink = void(*)(char, void*)`、`kprintf_register_sink(OutputSink, void* ctx)`(最多 8 路),`kprintf/kvprintf/kpanic` 遍历所有 sink 分发。

## 验证步骤

纯算术(下标公式、越界、PSF2 解析、光标状态机),host 单测镜像着测,不依赖 QEMU:

```bash
ctest --test-dir build -R 'font|framebuffer|console' --output-on-failure
```

注意这批单测是把你驱动里的公式**抄一份**到测试里测,不是直接调内核代码(内核代码在 host 上跑不起来)。它们绿,只代表算术对。

真硬件(显存映射、VBE 模式、字体真渲染)必须 QEMU 里验,用带测试钩子的内核:

```bash
cmake --build build --target run-big-kernel-test
```

机内测会:从 `BootInfo` 初始化 framebuffer 并断言 1024×768、pitch 合理;`put_pixel` 写一个像素、`get_pixel` 读回来一致、邻像素不受影响(这一条同时验证了显存真的映射进来了);字体渲染、console putc 与滚动。

想直接看「双输出」效果,`make run` 起来,留意那句 `Console initialised -- dual output active.`——它同时出现在串口和屏幕上,就说明 fan-out 通了。

## 常见故障

- **屏幕一直黑、串口有输出**:几乎总是装配顺序断了。沿依赖链倒查——`kprintf_init`(串口)→ `fb.init` → `font.init` → `console.init` → `register_sink(console)`,缺一不可。最阴的情况:`register_sink` 调了,但 console 的 `fb_` 还是 nullptr(fb 没 init),`putc` 开头的 nullptr 检查静默吞掉字符,不崩也不显示。
- **第一个 `put_pixel` 就 page fault**:显存没映射进来。先 kprintf 打出 `fb_addr` 看它在哪——如果是几 GB 开外的高地址,说明你得靠 1GB 大页那条路映射,检查 `CPUID` 探测和 PDPT 表项有没有真写进去。
- **画面歪斜、整体错位**:下标公式用了 `width` 而不是 `pitch`,或者漏了 `pitch/4` 的除法。
- **滚一次屏画面花了**:`scroll_up` 搬运的字节数算错(该用 `(height-lines)*pitch`,不是 `(height-lines)*width*4`),或清底部空带的高度传错。
- **字符只画出左半截 / 乱码**:字体宽度超过 8 但 `render_char` 还按「每行 1 字节」取位;或 PSF2 魔数没校验、header 字段解析错,字形指针偏了。
- **kprintf 屏幕这路没反应**:console sink 没注册,或注册了但传的 `ctx` 不是那个 console 实例(回调里 `static_cast` 回来的是错的指针)。

## 通过标准

1. host 单测全绿:下标公式、越界判断、PSF2 解析、光标/换行/滚动状态机都对。
2. QEMU 机内测通过:从 `BootInfo` 初始化出 1024×768、像素写进去读得回来、字体能画、console 能写能滚。
3. kprintf 双输出成立:`dual output active` 之后,诊断信息串口和屏幕同步,格式化引擎未被改动(还是 012 那份回调式引擎,只是输出分派从「单路」变成「多路 fan-out」)。
4. 装配顺序正确:fb、font、console 的初始化和 sink 注册严格按依赖链排,任何一环提前都会让你马上知道(黑屏或 page fault,而不是安静地错)。

做到这四条,内核就第一次有了「脸」——能在屏幕上说话了。但你会发现它还不会听:键盘敲进去的字符它一个都收不到。那是最先被接上真 handler 的输入设备,也是下一关的活。
