---
title: Lab 029 · 双缓冲画布:从像素到翻页
---

# Lab 029 · 双缓冲画布:从像素到翻页

> 029 引入了双缓冲画布 Canvas。这个 lab 把它拆成三层来吃:第一层是**画图原语**——像素/矩形/直线(Bresenham)的坐标、裁剪、越界,纯逻辑,host 单测就能压;第二层是**翻页机制**——flip 为什么要按 pitch 逐行拷、为什么目的地是 volatile、双缓冲怎么消除撕裂;第三层是**一次复盘**——canvas 一 init 就要 ~3 MB,这 3 MB 在 029 撞出了哪两个内存布局洞。没有新代码要写,是理解 + 推理 + 排错演练。

## 实验目标

- 用 host 单测压 `draw_pixel`/`draw_rect`/`draw_line`(Bresenham 各象限),覆盖坐标、裁剪、越界。
- 手算 back buffer 大小(1024×768×4 ≈ 3 MB)与 pitch 的含义,解释它**为什么**会触发 029 的堆越界。
- 解释 flip 的逐行 memcopy、`volatile` 目的地、pitch 对齐,以及双缓冲如何消除撕裂/闪烁。
- 理解 PIT tick callback → flip 的刷新时序,以及「业务自己 flip」与「节拍自动 flip」的取舍。
- 独立完成一次「canvas init 后内核 hang」的排错演练,写出三条假设并指出真因。

## 前置条件

- 028e 通过;理解帧缓冲(013)、PIT(011)、heap/VMM、028e 的 `memory_layout.hpp`。
- 能用 `CINUX_GUI=ON` 构建:`cmake -B build -DCINUX_GUI=ON && cmake --build build`。
- 读懂主书第 029 章的「Canvas 设计」「刷新与 demo」「调试现场」三节,以及 [029-canvas-heap-directmap.md](../../debug-notes/029-canvas-heap-directmap.md)。

## 任务分解

### 任务 1:host 单测压画图原语

`test/unit/test_canvas.cpp` 是 029 新增的 host 测试(纯 mock 实现,不需要特殊依赖),canvas 在 host 下用等价的 mock Framebuffer 跑。参考它,自己构造一组用例覆盖:

```text
draw_pixel:
  (0,0)、(width-1,height-1) 边角       → 写入正确索引
  越界 (width,0)、(0,height)、(-1 等价大值) → 应被裁掉，不写、不越界访问

draw_rect:
  全屏矩形、1×1 矩形、部分越界矩形       → 只画在界内的部分

draw_line (Bresenham)：
  水平线、垂直线、45°、陡斜率(>1)、
  负斜率(x1<x0 / y1<y0)               → 全八象限都要对
  起点等于终点                          → 退化成一个像素
```

关键是 Bresenham 的**负方向**(x1<x0 或 y1<y0)和**陡斜率**(|dy|>|dx|)——这俩最容易写错。如果你的 `draw_line` 只测了「右下方向」,就漏了一半象限。

> 接口约束:坐标都是 `uint32_t`(0 起,左上原点);颜色 `0x00RRGGBB`;越界一律静默丢弃,不返回错误。索引公式 `back_buf_[y * (pitch/4) + x]`。

### 任务 2:手算 back buffer,解释它为什么撞出堆越界

纸上算:

- 1024×768 屏,32 位像素,back buffer 多少字节?(1024 × 768 × 4 = 3 145 728 ≈ 3 MB)
- 028e 布局表里 `KMEM_HEAP_SIZE` 原先是多少?(1 MB)canvas 这 3 MB 比它大多少?
- heap 的 `expand()` 在 029 之前**有没有上限检查**?没有的话,这 3 MB 会让堆涨到哪个虚拟地址、踩进哪个区段?(提示:堆从 `KMEM_HEAP_BASE` 起,涨过 1 MB 就进了紧挨着的 MMIO / Stack 区段。)
- 029 的修复做了两件事,分别是什么?(heap 加 `max_size_` 上限 + `expand` 返回 bool;`KMEM_HEAP_SIZE` 提到 128 MB。)为什么「预留 128 MB 虚拟地址」不等于「立刻吃掉 128 MB 物理内存」?(物理页按需分配。)

这一步是把「一个 `new[]` 怎么搞挂内核」的因果链彻底走通。

### 任务 3:flip 的逐行拷贝与 volatile

读 `Canvas::flip()`,回答:

- 为什么是**逐行** `memcopy`(每行 `width*4` 字节、行间按 `pitch` 跨进),而不是一次性 `memcpy` 整块?(帧缓冲每扫描线字节数 pitch 可能 > `width*4`,有 padding。)
- 目的地为什么要 `reinterpret_cast<volatile uint8_t*>(front_buf_->data())`?`volatile` 在这里挡的是什么?(帧缓冲是 MMIO,阻止编译器把写入优化掉/合并。)
- 如果把 `width*4` 误写成 `pitch`(整行连 padding 一起从 back 拷到 front),画面会怎样?(back buffer 没有 padding,会把 padding 区的垃圾拷过去,错位/变色。)

### 任务 4:刷新时序——PIT tick callback

读 pit 的 `set_tick_callback` / `invoke_tick_callback` 和 main 里注册 flip 的那段,回答:

- flip 多久被调一次?(PIT 100 Hz → 每 10 ms 一次。)
- 「业务代码画完一帧自己 flip」和「把 flip 挂到节拍上自动翻」各有什么取舍?(自动翻省心、画面刷新稳定;但业务画图频率和翻页频率脱钩,可能翻到画一半的帧——本 lab 不展开,留作思考。)
- `invoke_tick_callback` 跑在什么上下文?(PIT 的 IRQ0 中断处理里。)它调到的 `flip` 会做一大段 memcopy,这在中断里是否理想?(不理想,长拷贝占着中断;029 这么做是简化,真实系统会用下半部/脏矩形。能指出这点就够。)

### 任务 5(排错演练):canvas init 后内核 hang

假设你刚把 canvas 接进内核,现象是:`CINUX_GUI=ON` 跑,canvas 测试之后内核 hang;`CINUX_GUI=OFF` 基线全过。请你独立写出**至少三条**排查假设,每条配验证手段,并指出哪条是真因。

参考方向(自己先写再看):

- 假设 A:堆越界(canvas 的大块分配让堆 expand 进了别的区段)——验证:在 heap expand 加打印看它涨到哪个虚拟地址,对照 `memory_layout.hpp` 看是否越过 `KMEM_HEAP_BASE + KMEM_HEAP_SIZE`。
- 假设 B:direct map 覆盖不足(大块分配耗尽低地址物理页,PMM 返回高地址,`phys_to_virt` 落在未映射处)——验证:在 `alloc_page` 后打印返回的物理地址,看是否超过 loader 的 identity-map 范围;或直接读 `phys_to_virt(高地址)` 是否 page fault。
- 假设 C:中断/抢占问题(flip 在 IRQ0 里跑,长拷贝被打断)——验证:在 flip 外包关中断看是否还 hang(028e/029 的真因不是这条,但要会**先证伪它**)。
- 真因:029 里是 A(heap 无上限)+ B(direct map 不足)两个**叠加**——先 A 后 B。

写完对照主书「调试现场」和 [029-canvas-heap-directmap.md](../../debug-notes/029-canvas-heap-directmap.md),看你的假设链是否覆盖了这两个洞、并正确地**按顺序**定位(先 heap 后 direct map)。

## 接口约束

- `cinux::drivers::Canvas::init(Framebuffer&)`:记尺寸 + 堆分配 back buffer(`width*height` 个 uint32)。
- `draw_pixel/draw_rect/draw_rect_outline/draw_line/draw_text/blit/clear`:全往 back buffer 写,坐标越界静默裁掉。
- `Canvas::flip()`:back → front 逐行 memcopy(volatile dst,按 pitch)。
- `cinux::drivers::PIT::set_tick_callback(cb, ctx)` / `invoke_tick_callback()`(CINUX_GUI):每 tick 回调。
- CMake:`CINUX_GUI`(默认 ON)控制 canvas 编译;`-DCINUX_GUI=OFF` 才关掉。

## 验证步骤

- **任务 1**:`ctest --test-dir build -R canvas --output-on-failure` 全绿;Bresenham 各象限用例尤其要过。
- **任务 2–4**:纸上完成;flip/pitch 的结论可在 `test_canvas` 的 flip 用例里对照自检。
- **任务 5**:写完假设链,对照 debug-notes 看是否覆盖 heap 越界 + direct map 两个洞。
- **端到端**:`cmake -B build -DCINUX_GUI=ON && cmake --build build && make run`,看到 demo(深靛背景 + 随机矩形 + 白色标题),画面稳定不闪。
- **回归**:`cmake -B build -DCINUX_GUI=OFF`(显式关掉 GUI)`&& ctest --test-dir build`,356 项基线全过。

## 常见故障

- **canvas.cpp 没编进去 / `Canvas` 未定义**:你显式 `-DCINUX_GUI=OFF` 关了 GUI。canvas 整个在 `#ifdef CINUX_GUI` 后面,关掉开关就不编 canvas(默认是开的,不传 `-DCINUX_GUI` 即可)。
- **`draw_line` 陡斜率 / 负方向画错**:Bresenham 的 `step_x/step_y` 符号或 `dx/dy` 取绝对值的顺序写错。八个象限都要测。
- **flip 后画面错位/变色**:`width*4` 误写成 `pitch`,或忘了按行跨进、一次性拷了整块。
- **GUI OFF 基线挂了**:布局改动越界了。查 `memory_layout.hpp`(`KMEM_HEAP_SIZE` 改动)和 loader 的 direct map,看是不是冲进了别的区段或映射范围算错。
- **canvas init 后 hang**:就是 029 的两个洞——heap 无上限、direct map 不足。先查 heap expand 有没有边界检查,再查 loader 是不是全量映射了物理内存。

## 通过标准

- 任务 1 画图原语用例全绿,Bresenham 八象限都对。
- 任务 2 能算出 back buffer ≈ 3 MB,并说清它为什么让无上限的堆 expand 踩进 MMIO/Stack。
- 任务 3 能解释 flip 逐行 + pitch + volatile 三个要点。
- 任务 4 能说出 PIT tick callback 的刷新频率与上下文,并指出「长拷贝在中断里」的隐患。
- 任务 5 写出至少三条假设(含 heap 越界 + direct map 两条真因),并按「先 heap 后 direct map」的顺序定位。
- `CINUX_GUI=ON` 的 `make run` 看到 demo;GUI OFF 基线 356 项全过。
- 能口头回答:双缓冲为什么消除撕裂?canvas 的 3 MB 为什么撞出堆上限?phys_to_virt 为什么依赖全量 direct map?
