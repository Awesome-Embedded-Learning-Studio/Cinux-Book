---
title: 029 · GUI 画布:双缓冲绘图,与它撞出的两个内存布局洞
---

# 029 · GUI 画布:双缓冲绘图,与它撞出的两个内存布局洞

> 028e 的结尾我们留了个引子:内核能调度、能阻塞了,帧缓冲(013)也早就点亮,下一步该**在屏幕上画东西**了。这一章就来画——引入一块双缓冲的「画布」(Canvas),提供像素、矩形、直线、文字这些基本绘图原语。但「画」这件事的第一步,就撞出了两个潜伏了好几个 tag 的内核虚拟内存布局问题:canvas 一初始化就要 ~3 MB 的后备缓冲,直接把堆撑爆、冲进了别的区段;等堆修好,又暴露出 loader 对物理内存的映射根本不够用。
>
> 所以这一章和 028e 是连着的:028e 把内核虚拟地址收拢成一张布局表,这一章则把这张表**压测**了一遍——发现「区段内部没有上限」和「direct map 覆盖不足」两个洞,都被 canvas 的大块分配逼了出来。读完你会有一块能画图的画布,外加两个关于「大块分配如何暴露布局缺陷」的真实复盘。

## 为什么是双缓冲画布

最朴素的画图方式是直接往帧缓冲(framebuffer)里写像素——013 点亮帧缓冲、console 往上打字,都是这么干的。但一旦你想画「一帧完整的画面」(一堆矩形、文字、再覆盖背景),直接写帧缓冲会有两个毛病:

- **撕裂(tearing)**:你画到一半时,显卡的扫描线正好扫到这一行,屏幕上就出现「上半帧旧、下半帧新」的裂缝。
- **闪烁(flicker)**:先擦再画的过程被肉眼看到,画面一闪一闪。

标准解法是**双缓冲**:另开一块和屏幕等大的内存当「后备缓冲」(back buffer),所有画图都画在它上面;一帧画完,再一次性把整块 back buffer 拷到硬件帧缓冲(front buffer)。这一拷是连续的一整块,扫描线撞上的概率小、即使撞上也是完整的一帧。这个「画完再翻」的动作叫 flip。这就是 Canvas 要做的事。

## Canvas 设计:front、back,和一套绘图原语

`kernel/drivers/canvas.hpp` 定义的 `Canvas`,核心就是两个缓冲指针加几个尺寸字段:

```cpp
class Canvas {
    Framebuffer* front_buf_ = nullptr;   // 硬件帧缓冲
    uint32_t*    back_buf_  = nullptr;   // 堆分配的后备缓冲（width*height 个 32 位像素）
    uint32_t     width_, height_, pitch_; // pitch = 每扫描线字节数
};
```

`init(fb)` 记下硬件帧缓冲的尺寸,然后**在内核堆上**分配一块 `width * height` 个 `uint32_t` 的 back buffer(32 位像素,`0x00RRGGBB`),清成黑色:

```cpp
void Canvas::init(Framebuffer& fb) {
    front_buf_ = &fb;
    width_  = fb.width();
    height_ = fb.height();
    pitch_  = fb.pitch();
    uint32_t total_pixels = width_ * height_;
    back_buf_ = new uint32_t[total_pixels];     // ← 就是这一句，要 ~3 MB
    memfill32(back_buf_, 0, total_pixels);
}
```

注意那句 `new uint32_t[total_pixels]`:1024×768 的屏幕,这就是 `1024 × 768 × 4 ≈ 3 MB`。记住这个数字,它是后面两个 bug 的导火索。

画图原语都是往 `back_buf_` 里写像素,坐标用 `pitch / 4`(每行多少个 32 位像素)来索引:

```cpp
void Canvas::draw_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (x >= width_ || y >= height_) return;        // 越界裁掉
    uint32_t pixels_per_row = pitch_ / 4;
    back_buf_[y * pixels_per_row + x] = color;
}
```

有了 `draw_pixel`,`draw_rect`(填充实心矩形)、`draw_rect_outline`(四条边)就是两层循环套 `draw_pixel`。`draw_line` 用经典的 **Bresenham 算法**——纯整数运算、处理全部八个象限、不涉及浮点,这是内核里画直线的标配:

```cpp
// Bresenham：用 err 累积判进，整数步进，覆盖所有斜率方向
int32_t dx = x1 - x0, dy = y1 - y0;
int32_t step_x = (dx >= 0) ? 1 : -1, step_y = (dy >= 0) ? 1 : -1;
dx = (dx >= 0) ? dx : -dx;  dy = (dy >= 0) ? dy : -dy;
int32_t err = dx - dy;
while (true) {
    draw_pixel(cx, cy, color);
    if (cx == x1 && cy == y1) break;
    int32_t e2 = 2 * err;
    if (e2 > -dy) { err -= dy; cx += step_x; }
    if (e2 <  dx) { err += dx; cy += step_y; }
}
```

`draw_text` 拿 `PSFFont` 的 glyph 数据,逐像素把每个字符点阵「点」到 back buffer 上(超出的字符裁掉)。`blit` 则是从另一块 canvas 拷一块矩形区域过来——这是后面做窗口/贴图的基础。

最后是 **flip**:把 back buffer 逐行拷到硬件帧缓冲。注意目的地是 `volatile`,因为帧缓冲是 MMIO,编译器不能把写入优化掉:

```cpp
void Canvas::flip() {
    auto* dst = reinterpret_cast<volatile uint8_t*>(front_buf_->data());  // 硬件帧缓冲（MMIO）
    auto* src = reinterpret_cast<const uint8_t*>(back_buf_);
    for (uint32_t row = 0; row < height_; row++) {
        memcopy(dst + row * pitch_,            // 每行按 pitch（每扫描线字节数）对齐
                src + row * pitch_,
                width_ * 4);
    }
}
```

为什么要「逐行按 `pitch` 拷」而不是一次性 `memcpy` 整块?因为帧缓冲的**每扫描线字节数(pitch)**不一定等于 `width * 4`——硬件可能因对齐/缓存行在每行末尾塞 padding。逐行拷、每行拷 `width * 4` 字节、行间按 `pitch` 跨进,才不会把 padding 搞乱。

整个 canvas 只在 `CINUX_GUI` 这个 CMake 开关打开时才编译(注意:这个开关**默认就是开的**,要显式 `-DCINUX_GUI=OFF` 才关掉):

```cmake
if(CINUX_GUI)
    target_compile_definitions(big_kernel_common PUBLIC CINUX_GUI)
    ...
    drivers/canvas.cpp
```

也就是说,GUI **默认就是开的**;只有显式 `-DCINUX_GUI=OFF` 才把画图能力摘掉,让 Cinux 退回纯命令行内核。

## 刷新与 demo:把 flip 挂到时钟节拍上

有了 canvas,「谁来调 flip」是下一个问题。业务代码自己画完自己 flip 当然可以,但更省事的做法是**把 flip 挂到一个固定节拍上**,让画面自动刷新。Cinux 借的是 PIT:011 那个 100 Hz 的时钟,现在多了一个「每 tick 回调」的口子(同样只在 `CINUX_GUI` 下编译):

```cpp
// pit.hpp
static void set_tick_callback(void (*cb)(void*), void* ctx = nullptr);
static void invoke_tick_callback();   // irq0_handler 每次中断里调
```

`main.cpp` 里(GUI 开启时)就接上了:建一块 canvas、画个 demo、然后把它的 flip 注册成 tick 回调:

```cpp
Canvas g_canvas;
g_canvas.init(fb);
g_canvas.clear(0x001A1A2E);                 // 深靛色背景
for (/* 一批 */) {
    uint32_t x = lcg_next() % (g_canvas.width()  - 100);
    uint32_t y = lcg_next() % (g_canvas.height() - 60);
    g_canvas.draw_rect(x, y, w, h, color);   // LCG 伪随机矩形
}
g_canvas.draw_text(text_x, 10, title, 0x00FFFFFF, font);  // 白色标题
g_canvas.flip();

PIT::set_tick_callback([](void* ctx) {
    static_cast<Canvas*>(ctx)->flip();
}, &g_canvas);
```

这里的 `lcg_next()` 是个**确定性**的线性同余伪随机数(内核里没有 `rand()`),所以 demo 画出的那批矩形每次启动都一样——这在没有真随机的内核里是预期行为,不是 bug。注册完回调后,PIT 每 10 ms 触发一次 `g_canvas.flip()`,画面就持续把 back buffer 翻到屏幕。这条刷新链画出来是:

```text
PIT IRQ0（每 10 ms）
   └─ irq0_handler
        ├─ tick_count_++（atomic）
        ├─ PIC::send_eoi(0)
        └─ invoke_tick_callback()
              └─ g_canvas.flip()   ← back buffer 逐行 memcopy 到 硬件帧缓冲
```

## 调试现场:canvas 的 3 MB,撞出两个布局洞

模型很干净,一开 GUI 跑——挂了。而且是两种不同的挂法,分别对应两个早就潜伏的内存布局问题。这两个问题是这一章的主体,因为它们把 028e 那张布局表真正的薄弱处逼了出来。

### 第一个洞:堆可以无限扩展,canvas 把它撑进了别人的地盘

**现象**:`CINUX_GUI=ON` 时,canvas 相关测试全过,但紧接着的 `test_fifo_ordering` 在创建一个任务时 hang 住。`CINUX_GUI=OFF` 时,356 项基线测试一个不少全过。换句话说,挂是 GUI 引入的。

**根因**。还记得 `Canvas::init` 那句 `new uint32_t[1024*768]` 吗?它要 ~3 MB。可 028e 的布局表里,堆区段 `KMEM_HEAP_SIZE` 只**预留**了 1 MB——而这 1 MB 只是「初始大小」,堆的 `expand()` 在没找到合适空闲块时会**自动扩容**,而且**没有上限检查**。于是 canvas 这 3 MB 一来,堆就一路 expand,从 1 MB 涨过 2 MB、3 MB……直接冲出了堆区段、踩进了后面紧挨着的 MMIO / Stack 区段的虚拟地址。后面的 `TaskBuilder` 再去给新任务映射内核栈时,`g_vmm.map()` 的行为就乱了——于是 hang。

讽刺的是:这正是 028e 修掉的「栈盖 MMIO」那类问题的**变体**。028e 把区段边界集中管起来了,但**区段内部**「堆能涨到多大」并没有限制——堆以为自己头顶上有一片无限大的虚拟空间,其实隔壁就是 MMIO 和栈。

**修复**,两件事一起做:

1. 给堆一个**硬上限**。`heap.hpp` 加 `max_size_` 字段,`expand()` 改成返回 `bool`,扩容前先查边界:

   ```cpp
   bool Heap::expand(size_t min_bytes) {
       // 边界检查：不许越过预留的堆区段
       if (size_ + expand_size > max_size_) {
           kprintf("[HEAP] Expansion limit reached: %u KB / %u MB\n", ...);
           return false;
       }
       ...
       return true;
   }
   ```

   `alloc` 拿到 `expand` 失败就老老实实返回 `nullptr`,不再递归重试。

2. 把堆区段**真正预留大**。`KMEM_HEAP_SIZE` 从 1 MB 提到 **128 MB**(`0x8000000`)。GUI 的画面缓冲、控件、贴图都要堆,1 MB 根本不够;128 MB 是个 GUI 内核的合理预留(物理页是按需分配的,预留虚拟地址不浪费物理内存)。

### 第二个洞:phys_to_virt 依赖一张「覆盖不全」的 direct map

堆修好了,再跑——又 hang,这次是 `test_create_user_space`。

**根因**。内核里有个习惯用法 `phys_to_virt(p) = p + KERNEL_VMA`(`KERNEL_VMA = 0xFFFFFFFF80000000`):把一个物理地址变成内核能直接访问的虚拟地址。`AddressSpace` 构造时就这么干。但这套用法有个**隐含前提**:那个 `phys + KERNEL_VMA` 的虚拟地址,得真的被映射过。

偏偏 loader(小内核加载大内核那段)之前只 `identity_map` 到了 ELF 段末尾,大约 ~20 MB。可 PMM 管着 **9 GB** 物理内存,`alloc_page()` 可能返回任意物理地址。一旦它返回一个 > 20 MB 的页,`phys_to_virt` 算出来的虚拟地址就**没人映射**,一访问就 page fault。canvas 那一堆大块分配正好把低地址物理页消耗得差不多了,后面的分配就更容易落到高地址——于是这个潜伏问题被「喂」了出来。

**修复**:让 loader 把**全部物理内存**都映射到 `KERNEL_VMA` 起的那段(Linux 风格的 full direct map),这样 `phys_to_virt` 对 PMM 返回的任何页都成立:

```cpp
// big_kernel_loader.cpp phase2：扫 E820 找最高可用 RAM，全量映射
for (uint32_t i = 0; i < bi->mmap_count; i++) {
    if (bi->mmap[i].type == 1)            // usable RAM
        highest_phys = max(highest_phys, bi->mmap[i].base + bi->mmap[i].length);
}
// 用 2MB 大页映射低段、1GB 大页映射高段（全量 direct map，开销极小）
```

用**大页**(2 MB、1 GB)映射是关键:全量映射 9 GB 物理内存,如果用 4 KB 页,光页表就要一大堆;用 2 MB / 1 GB 大页,几个页表项就盖住了(8 GB 量级大约只需约 5 个页表页)。映射全部 RAM 之后,`test_demand_page` 原本「依赖某地址未映射来触发 page fault」的假设失效了,也得相应改成「验证高地址能正常读写」。

### 修完后的布局

两个洞补上之后,028e 那张布局表长成了这样(地址取自源码 `memory_layout.hpp` 和排查 note):

```text
0xFFFF8000_00000000  KMEM_HEAP_BASE      128 MB 预留（按需分配物理页）
0xFFFF8000_08000000  KMEM_MMIO_BASE      256 KB（AHCI BAR5 等）
0xFFFF8000_08040000  KMEM_STACK_BASE     1 MB（每 task 4 页往上长）
0xFFFF8000_08140000  KMEM_DMA_BASE       1 MB
0xFFFF8000_08240000  KMEM_EXT2_DMA_BASE  1 MB
...
0xFFFFFFFF_80000000  KERNEL_VMA          全量物理内存 direct map
```

> 把两个洞放一起看,教训是同一条:**「地址布局」不只是「区段别重叠」,还包括「区段内部要有边界」和「direct map 要覆盖 PMM 可能返回的所有地址」**。028e 解决了第一层,canvas 的大块分配把后两层也逼了出来。这类问题在单测里看不见——单测不会真去分配 3 MB——只有像 canvas 这样的大块消费者,或者内核集成测试,才暴露得了。

## 验证

GUI 默认就是开的,所以常规构建就带画图能力(只要没显式 `-DCINUX_GUI=OFF`):

```bash
cmake -B build              # CINUX_GUI 默认 ON
cmake --build build
```

**画图单测**(纯像素/坐标/裁剪逻辑,host 上跑):

```bash
ctest --test-dir build -R canvas --output-on-failure
```

`test_canvas` 覆盖 `draw_pixel`/`draw_rect`/`draw_line`(水平、垂直、45°、陡斜率、单点这几个**正方向**用例)/`draw_text`/裁剪/`flip` 的逐行拷贝这些纯逻辑,host 上就能验全(负方向的画线它没覆盖,留给 lab 任务 1 自己补)。kernel 端(`run-kernel-test`)还有一份在 QEMU 里跑的 canvas 测试。

**端到端看画面**:

```bash
make run      # CINUX_GUI=ON 的构建
```

预期看到:深靛色背景、一批随机大小的彩色矩形、顶部一行白色标题文字——就是 `main.cpp` 里那段 demo。PIT 每 10 ms 翻一次页,画面稳定不闪。

**关键回归**:GUI 显式关掉(`-DCINUX_GUI=OFF`)重跑,356 项基线测试仍应全过——证明 canvas 引入的堆/direct map 改动没伤到非 GUI 路径。如果 GUI OFF 基线挂了,多半是布局表(heap 上限、direct map)改越界了,回去查 `memory_layout.hpp` 和 loader。

## 下一站

029 给了我们一块能在上面画像素、矩形、直线、文字的双缓冲画布——一块**平面**的、铺满全屏的绘图表面。但「桌面」不是一块平面画布:你要能同时开好几个窗口、它们能重叠、能移动、被遮住的区域不该被乱画。下一步(030)要做的,就是在这块平面画布之上,搭一套**窗口管理**——把「谁画在哪、谁盖着谁」管起来。具体怎么管理窗口和叠放,那是 030 的事。

---

**参考**

- 双缓冲与页面翻转(tearing、flicker):canvas 的 back buffer + flip 动机。可参考任意图形学教材对 double buffering / page flipping 的说明,或 OSDev Frame Buffer。
- Bresenham 直线算法:`Canvas::draw_line` 的整数、全象限实现依据。经典计算机图形学算法。
- PSF / PSF2(PC Screen Font)字体格式:`draw_text` 用的 `PSFFont` glyph 数据来源。OSDev PC Screen Font:<https://wiki.osdev.org/PC_Screen_Font>。
- Linux 内核 direct map / `phys_to_virt`(`phys + PAGE_OFFSET`/`__va`):loader 全量映射物理内存的参照模型。<https://www.kernel.org/doc/html/latest/core-api/mm-api.html>
- x86-64 大页(2 MB PD、1 GB PDP):全量 direct map 用大页低开销的依据。Intel SDM Vol 3(分页)/ OSDev Page Table。<https://wiki.osdev.org/Page_Table>
- E820 BIOS memory map:loader 扫描「最高可用 RAM」的依据。OSDev E820:<https://wiki.osdev.org/Detecting_Memory_(x86)>。
