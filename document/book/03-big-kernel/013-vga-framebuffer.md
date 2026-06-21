---
title: 013 · 把字写到屏幕上:VBE 线性帧缓冲与 PSF2 字体
---

# 013 · 把字写到屏幕上:VBE 线性帧缓冲、MMIO 映射与 PSF2 字体

> 012 那一章我们夯实了两个地基:kprintf 终于像个 printf,引导期 SSE 也修好了。但有个尴尬一直没解决——内核说的话,只走串口一条路。你得专门开个串口窗口才看得见它在嘀咕什么,屏幕始终是黑的。这一章,我们要让内核**直接在屏幕上画出像素、画出字**。为此得搭三层东西:一块能写的显存(framebuffer)、一个能把字变成点阵的字体,以及夹在中间、把那块物理显存搬进内核虚拟地址空间的那一步映射。这一章先讲前两层加上那步映射;把像素变成「能换行、能滚动」的文本终端,以及把 kprintf 接到屏幕上,留到 013b。

## 这一章我们要点亮什么

两件看得见的事。

第一件,内核启动后屏幕不再是黑的。bootloader 会把显卡切到一个 1024×768、每像素 32 位的图形模式,把一块连续的物理显存(线性帧缓冲,linear framebuffer)交给我们。我们只要往这块显存里写一个 32 位数,屏幕上对应位置就亮起一个像素。

第二件,内核能在这块显存上**画字**。光能画点不够——诊断信息总不能一个点一个点地拼。我们内置了一个 8×16 的点阵字体(PSF2 格式),每个字符就是 16 个字节、每字节 8 位,哪位是 1 哪个像素就亮。于是 `'A'` 不再是个抽象的码,而是一组实实在在的像素。

为了这两件事能成立,中间还卡着一个容易被忽略、但写错就一定 page fault 的环节:那块显存在**物理地址**空间里,内核跑在**虚拟地址**空间里,你得先把这段物理显存映射进内核能访问的虚拟地址,后面所有的写像素才有效。这一步我们用一个最小的页表助手 `map_mmio` 搞定,它也是这一章技术含量最高的部分。

## 为什么现在需要它

先说为什么是现在。到 012 为止,内核已经能在 `sti` 之后稳定响应时钟中断,异常链也走通了。但系统的「脸」还是黑的。接下来无论是想做个 shell、想画个简单的图形界面,还是仅仅想在调试时**不依赖串口窗口**就能看见内核在说什么,都绕不开「先把屏幕点亮」这一步。它是第一个真正「看得见」的驱动,也是后面所有用户态交互的舞台。

再往回看一层,这一章还顺手兑现了 012 埋下的一个承诺。012 把 kprintf 的格式化引擎和输出后端用回调解耦时,我们说过「013 之后还会多一路屏幕,到那时你就明白这层回调当初为什么要抽出来了」。这一章先把屏幕这路硬件备好(像素能画、字能显);真正把 kprintf 接上来、让那句承诺落地,是 013b 的事。所以这两章是一条线的两段:013 解决「屏幕能不能画」,013b 解决「kprintf 说不说得到屏幕上」。

## 设计图

从 bootloader 把显卡切到图形模式,到内核画出一个像素,中间是三层栈:

```text
  ┌──────────────────────────────────────────────────────────┐
  │  bootloader (实模式, VBE INT 0x10)                        │
  │  设模式 0x144 = 1024x768x32 + LFB bit14                   │
  │  → 把 fb_addr/fb_width/fb_height/fb_pitch/fb_bpp 塞进     │
  │    BootInfo (物理 0x7000)                                 │
  └────────────────────────────┬─────────────────────────────┘
                               ▼  big kernel 读 BootInfo
  ┌──────────────────────────────────────────────────────────┐
  │  arch::map_mmio(fb_phys, fb_size)        ← 本章难点       │
  │  在 bootloader 留下的页表里补条目, 把 [phys, phys+size)    │
  │  恒等映射 (virtual == physical), 用 2MB / 1GB 大页        │
  └────────────────────────────┬─────────────────────────────┘
                               ▼  现在 fb_phys 这个物理地址
                                  可以当虚拟地址直接访问了
  ┌──────────────────────────────────────────────────────────┐
  │  Framebuffer                                              │
  │  addr_ = (volatile uint32_t*)fb_phys                      │
  │  put_pixel(x,y,argb): addr_[y*(pitch/4) + x] = argb       │
  └────────────────────────────┬─────────────────────────────┘
                               ▼  能画点了, 但点不等于字
  ┌──────────────────────────────────────────────────────────┐
  │  PSFFont                                                  │
  │  assets/font.psf (.incbin 嵌进内核) → PSF2 header + 字形  │
  │  render_char(fb,'A',x,y,fg,bg): 逐位 put_pixel            │
  └──────────────────────────────────────────────────────────┘
```

最关键、也最容易写错的是中间那层 `map_mmio`。很多人第一次 framebuffer 不亮,都不是算错像素坐标,而是根本没把显存映射进来——往一个没映射的虚拟地址写,直接 page fault。所以这一章的「代码路线」会从那一步讲起,而不是急着画像素。

## 代码路线

### bootloader 把模式定下来:VBE 0x144 + 线性帧缓冲

屏幕能画东西的前提,是显卡先被切到一个图形模式。这件事发生在 bootloader 还在实模式、能用 BIOS 的阶段,靠的是 VESA VBE(VGA BIOS Extension)那套 `INT 0x10` 调用。

具体到 Cinux,这部分代码在 [serial.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/common/serial.S)。没错,文件名叫 `serial.S`,里头却装着 VBE 的活——这是历史包袱,别被名字带偏。它干两件事:先 `INT 0x10 AX=0x4F01` 取某个模式的信息(分辨率、显存物理地址、pitch),再 `INT 0x10 AX=0x4F02` 设置这个模式。设模式时,模式号要**或上 `0x4000`(bit 14)**,这一位表示「我要线性帧缓冲」(linear framebuffer)——也就是让显卡把整块显存以一段连续物理地址暴露出来,而不是老式 VGA 那种分段 bank 切换的访问方式。没有这一位,你拿不到一个能直接随机写的平坦地址。

这一章对这个 tag 的改动,其实只有一行常数和一处注释:

```asm
-.set VESA_TARGET_MODE,         0x4118  // 0x118 with bit14 set (linear framebuffer)
+.set VESA_TARGET_MODE,         0x4144  // 0x144 (1024x768x32, Bochs VBE) with bit14 set
```

也就是说,VBE 这套机制更早的 boot tag 就搭好了,本章只是把目标模式从 `0x118`(1024×768×24)重定向到 `0x144`(1024×768×32),顺手把 `vesa_set_mode` 顶部那条 `// TODO: Step 1: Call BIOS INT 0x10 ...` 注释改成了描述性注释。注意是「改注释」,不是「填代码」——函数体里那条 `int $0x10` 从 012 就在那儿,013 一字没动,真正变的只有模式号常数和这一行注释。所以别误以为「VBE 模式切换是 013 新加的」——它早就在,013 只是换了个更高位的模式号、清掉了那条已经过时的 TODO 注释。

模式设好之后,bootloader 把这块显存的情报写进一个结构体 `BootInfo`,放在物理地址 `0x7000`:

```c
typedef struct {
    ...
    uint64_t fb_addr;      // 显存物理基地址
    uint32_t fb_width;     // 像素宽
    uint32_t fb_height;    // 像素高
    uint32_t fb_pitch;     // 每扫描线字节数(可能 > width*4)
    uint32_t fb_bpp;       // 每像素位数(这里是 32)
} BootInfo;
```

big kernel 启动后第一件事之一,就是去 `0x7000` 把这份情报读出来。注意 `fb_pitch` 这个字段:它是**每条扫描线的字节数**,不一定等于 `width × 4`——显卡可能为了对齐在每行末尾补几个字节。后面算像素下标时,要用 `pitch` 而不是 `width`,这是新手最容易踩的第一个坑。

### 先得让那块显存能访问:map_mmio 的恒等映射

情报有了,但你现在还**碰不到**那块显存。`fb_addr` 是个物理地址(在 QEMU 的 Bochs VBE 下,这块显存通常位于很高的物理地址,几 GB 开外),而内核跑在虚拟地址空间里。你直接 `(uint32_t*)fb_addr` 去解引用,要么 page fault,要么写到一个毫不相干的地方。

所以画像素之前,必须先把 `[paging.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/paging.cpp) 里 `map_mmio` 的活:

```cpp
void map_mmio(uint64_t phys, uint64_t size);
```

它怎么映射,是这一章最值得讲清楚的地方。先看它最终要达到的效果:**映射完之后,`fb_addr` 这个物理地址,可以直接当成虚拟地址来用**。也就是说,内核里写 `((volatile uint32_t*)fb_addr)[i] = color`,就能改到屏幕上对应的像素。这意味着 `map_mmio` 做的是一段**恒等映射**(identity mapping):虚拟地址 `V` 指向物理地址 `V`。

这点有点反直觉——内核不是通常跑在「高半区」(virtual = phys + 某个大偏移)吗?确实。但帧缓冲这块我们特意让它恒等,是为了简单:`Framebuffer::init` 里就能直接 `addr_ = (volatile uint32_t*)fb_phys`,不用再算什么偏移。代价是 `map_mmio` 得在页表里专门为这段物理地址补上「virtual == physical」的条目。

那它怎么补条目?这里有个前提:bootloader 和 mini kernel 早就把一套页表建好了,big kernel 是踩在它们肩膀上跑的。`map_mmio` 不建新表,而是**直接去改那张已存在的页表**,往里补几个条目。它怎么找到那张表?靠两个写死的虚拟地址:

```cpp
constexpr uint64_t PD_VIRT_ADDR   = 0xFFFFFFFF80003000ULL;  // 页目录 (PD, 2MB 粒度)
constexpr uint64_t PDPT_VIRT_ADDR = 0xFFFFFFFF80002000ULL;  // 页目录指针表 (PDPT, 1GB 粒度)
```

这两个地址是 bootloader/mini kernel 当初放页表的地方(并且把它们映射到了高半区,所以内核能按这两个虚拟地址访问到表本身)。`map_mmio` 就把这两个地址当指针,直接改表项。这是一个**最小、有效、但脆弱**的方案:它假设页表就在这两个固定位置、假设表的结构是标准的 4 级分页。等以后(015、016 那一带)我们做了正经的页表管理器,这种「硬编码地址摸进页表」的写法会被替换掉。但此刻,为了点亮屏幕,它够用,而且干净。

补条目的逻辑分两段,对应两种大页粒度:

```cpp
// 第一段:物理地址 < 1GB 的部分,用 2MB 大页(PD 表项)
constexpr uint64_t PD_HUGE_PAGE_FLAGS = 0x83;   // P(bit0) + R/W(bit1) + PS(bit7)
uint64_t cur = phys & ~(PAGE_2MB_SIZE - 1);     // 对齐到 2MB
while (cur < end && cur < PAGE_1GB_SIZE) {
    uint32_t idx = cur / PAGE_2MB_SIZE;          // PD 里第几个表项
    if (idx < PT_ENTRIES && pd[idx] == 0) {      // 只补空位, 不覆盖已有映射
        pd[idx] = cur | PD_HUGE_PAGE_FLAGS;      // virtual = idx*2MB → physical = cur
        __asm__ volatile("invlpg (%0)" : : "r"(cur));  // 作废该页的 TLB
    }
    cur += PAGE_2MB_SIZE;
}
```

这里每一行都值得说一下。`0x83` 这个标志位是 `P(present,bit0) + R/W(read-write,bit1) + PS(page-size,bit7)`,PS 位置 1 表示这是一个 2MB 大页(而不是指向下一级页表的指针)。`pd[idx] = cur | flags` 这一句是核心:它把「物理地址 `cur`」填进 PD 的第 `idx` 项。而 PD 的第 `idx` 项,管的就是**虚拟地址 `idx × 2MB`**(因为 PD 这一级每个条目覆盖 2MB 虚拟空间)。由于 `idx = cur / 2MB` 且 `cur` 已 2MB 对齐,所以 `idx × 2MB == cur`——虚拟地址 `cur` 指向物理地址 `cur`,恒等映射就这么成了。每补一项紧跟一个 `invlpg`,作废这条地址在 TLB 里的旧缓存,免得 CPU 还拿「没映射」的旧认知去访问。

第二段处理物理地址 ≥ 1GB 的部分,粒度升到 1GB 大页(PDPT 表项):

```cpp
// 第二段:物理地址 >= 1GB 的部分, 用 1GB 大页(PDPT 表项), 需要CPU支持
constexpr uint64_t PDPT_1GB_PAGE_FLAGS = 0x83;  // 同样 P+RW+PS
if (end > PAGE_1GB_SIZE && has_1gb_pages()) {
    ...
    pdpt[n] = cur1g | PDPT_1GB_PAGE_FLAGS;      // virtual = n*1GB → physical = cur1g
    ...
    reload_cr3();   // 1GB 页改动后整体刷新 CR3
}
```

为什么 ≥1GB 要换 1GB 粒度?因为 PD 这张表只覆盖前 1GB 的虚拟地址(VA 的某几位索引 PD),再往上得去动 PDPT 这一级。而 1GB 大页是个**可选**特性,不是所有 x86-64 都有,所以这里先 `has_1gb_pages()` 探一下:

```cpp
bool has_1gb_pages() {
    uint32_t eax = 0x80000001, edx;
    __asm__ volatile("cpuid" : "+a"(eax), "=d"(edx) : : "ebx", "ecx");
    return (edx & (1u << 26)) != 0;   // CPUID.80000001h:EDX[26] = PDPE1GB
}
```

`CPUID` 扩展叶 `0x80000001` 的 `EDX` 第 26 位(`PDPE1GB`)为 1,才表示 CPU 支持 1GB 大页。这一位的探测在「调试现场」里会再提一次,因为它和帧缓冲能不能亮直接相关。

把这两段合起来看,`map_mmio` 的本质就是:**在已知位置的页表里,为 `[phys, phys+size)` 这段物理内存补上恒等映射的大页条目,<1GB 用 2MB 页、≥1GB 用 1GB 页,然后刷 TLB/CR3 让它生效**。它不分配新表、不做权限精细管理、不管并发——是个彻头彻尾的「够用就好」的最小助手。但对点亮屏幕这一步,它就是那把钥匙。

### Framebuffer:一块能随机写的显存

映射好了,[framebuffer.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/video/framebuffer.cpp) 这层反而简单——它就是把这块显存当成一个 `uint32_t` 数组来用:

```cpp
void Framebuffer::init(const BootInfo& bi) {
    uint64_t fb_phys = bi.fb_addr;
    width_  = bi.fb_width;  height_ = bi.fb_height;
    pitch_  = bi.fb_pitch;  bpp_    = bi.fb_bpp;

    uint64_t fb_size = static_cast<uint64_t>(pitch_) * height_;
    arch::map_mmio(fb_phys, fb_size);                 // ← 上一步的映射

    addr_ = reinterpret_cast<volatile uint32_t*>(fb_phys);  // 物理地址当虚拟地址用
}
```

注意 `fb_size = pitch_ * height_`,用的是 `pitch` 不是 `width`——因为每行实际占 `pitch` 字节(含补齐),整块显存大小得按 `pitch` 算。映射的尺寸算少了,后面写像素就会越界到没映射的区域。

画一个像素,核心就一个下标公式:

```cpp
void Framebuffer::put_pixel(uint32_t x, uint32_t y, uint32_t argb) {
    if (x >= width_ || y >= height_) return;          // 越界直接丢弃
    addr_[y * (pitch_ / 4) + x] = argb;
}
```

`addr_` 是 `uint32_t*`,所以下标按「32 位单元」计;而 `pitch` 是**字节数**,得 `/4` 换算成「每行几个像素单元」。于是第 `y` 行第 `x` 个像素,就是这个公式。这个 `pitch/4` 是第二个新手坑:如果你下意识写成 `y * width_ + x`,一旦显卡的 `pitch > width*4`(很常见,为了对齐),画面就会整体歪斜、错位。

颜色 `argb` 是 `0x00RRGGBB`(高字节 0x00 在大多数 32bpp 模式下是忽略或作 alpha)。`fill_rect` 就是两层循环套 `addr_[...] = argb`,`clear` 是填满。`get_pixel` 反过来读,给测试回读用。

稍微值得一提的是滚动 `scroll_up`,因为后面的文本控制台靠它翻页:

```cpp
void Framebuffer::scroll_up(uint32_t lines, uint32_t line_height, uint32_t bg) {
    if (lines >= height_) { clear(bg); return; }
    auto* buf = reinterpret_cast<volatile uint8_t*>(addr_);
    const volatile uint8_t* src = buf + pitch_ * lines;   // 从第 lines 行起
    volatile uint8_t*       dst = buf;                    // 搬到最顶
    uint32_t move_bytes = (height_ - lines) * pitch_;
    for (uint32_t i = 0; i < move_bytes; i++) dst[i] = src[i];  // 字节级 memmove
    fill_rect(0, height_ - line_height, width_, line_height, bg);  // 清底部空带
}
```

它把整块显存当**字节**数组,把第 `lines` 行以后的内容整体往上搬 `lines` 行,再 `fill_rect` 把底部露出来的空带清成背景色。这里按字节搬而不是按像素搬,是因为「行」这个概念只在文本层有意义,对裸显存来说它就是一段连续字节,字节级搬运最直接。代价是 O(显存大小) 的拷贝——对一个控制台来说完全可接受。

### PSF2 字体:从 .py 生成到 .incbin 嵌进内核

能画点了,但点不等于字。要把 `'A'` 画出来,得有一张「字符 → 点阵」的表,也就是字体。Cinux 用的是 PSF2(PC Screen Font v2)格式,一个 8×16、256 个字形的小字体。

这个字体不是手写的,是用脚本生成的——[gen_psf_font.py](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/scripts/gen_psf_font.py)。它把经典的 IBM PC 8×16 CP437 点阵数据,按 PSF2 格式写进 `assets/font.psf`。PSF2 文件长这样:先一个 32 字节的 header,再紧跟每个字形的数据。

```python
PSF2_MAGIC       = 0x864AB572   # PSF2 魔数
PSF2_HEADER_SIZE = 32
PSF2_LENGTH      = 256          # 256 个字形
PSF2_CHARSIZE    = 16           # 每字形 16 字节(16 行 × 1 字节/行)
PSF2_HEIGHT      = 16
PSF2_WIDTH       = 8
```

每个字形 16 字节,正好对应 16 行,每行 1 字节、8 个位,哪位是 1 哪个像素就亮——宽度 8 刚好塞进一个字节。比如 `'!'`(0x21)的点阵是 `[0x18,0x18,0x18,...]`,第一行 `0x18 = 0001_1000`,中间两个像素亮,正是一个感叹号上半段的竖线。

生成出来的 `assets/font.psf` 是个二进制文件,得想办法把它带进内核镜像。这里用的是汇编的 `.incbin` 指令,在 [font_data.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/video/font_data.S) 里把它直接嵌进内核的 `.rodata` 段:

```asm
.section .rodata
.global font_psf_start
font_psf_start:
    .incbin "assets/font.psf"     ; 把整个二进制文件原样塞进来
.global font_psf_end
font_psf_end:
.global font_psf_size
font_psf_size:
    .long font_psf_end - font_psf_start   ; 4 字节小端长度
```

`.incbin` 是 GAS 的指令,作用类似 `.incbin "文件"`——把指定文件的内容一字不差地拼进当前段,并导出起始/结束符号。于是 C++ 侧就能拿到这段数据的指针:

```cpp
extern "C" {
extern const uint8_t  font_psf_start[];
extern const uint8_t  font_psf_end[];
extern const uint32_t font_psf_size[];
}
```

字体驱动 [font.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/video/font.cpp) 拿这个指针,先解析 header:

```cpp
static constexpr uint32_t PSF2_MAGIC = 0x864AB572;

void PSFFont::init() {
    const auto* hdr = reinterpret_cast<const PSF2Header*>(font_psf_start);
    if (hdr->magic != PSF2_MAGIC) return;     // 魔数不对就什么都不画(安全退化)
    num_glyphs_     = hdr->length;            // 256
    bytes_per_glyph_= hdr->charsize;          // 16
    width_          = hdr->width;             // 8
    height_         = hdr->height;            // 16
    glyphs_         = font_psf_start + hdr->header_size;  // header 之后就是字形数据
}
```

魔数校验是个小细节但很重要:`.incbin` 嵌进来的东西对不对,运行时没人替你把关。`hdr->magic != PSF2_MAGIC` 时直接 `return`,让 `glyphs_` 保持 `nullptr`,后面 `render_char` 见到 `nullptr` 就什么都不画——比起乱解引用崩掉,这种「安全退化」对一个显示驱动是合理选择。

最后是画字,逐行逐位地把字形转成像素:

```cpp
void PSFFont::render_char(Framebuffer& fb, uint8_t c, uint32_t x, uint32_t y,
                          uint32_t fg, uint32_t bg) {
    if (glyphs_ == nullptr) return;
    if (c >= num_glyphs_) c = 0;                       // 越界字形退化到 0 号
    const uint8_t* glyph = glyphs_ + c * bytes_per_glyph_;
    for (uint32_t row = 0; row < height_; row++) {
        uint8_t bits = glyph[row];                     // 这 1 字节就是这一行的 8 个像素
        for (uint32_t col = 0; col < width_; col++) {
            bool on = (bits >> (7 - col)) & 1;         // 最高位是最左边的像素
            fb.put_pixel(x + col, y + row, on ? fg : bg);
        }
    }
}
```

`(bits >> (7 - col)) & 1` 这一句里,`col=0` 取最高位(bit7),对应最左边的像素——这和 PSF 格式「高位在左」的约定一致。`on ? fg : bg` 同时画了前景和背景,所以每个字符都是一个完整的 8×16 色块,不会和旁边重叠的字符糊在一起。

这里有个隐含的边界条件:循环里 `bits` 是个 `uint8_t`、`glyph[row]` 每行只取 1 字节,所以这套写法**只对宽度 ≤ 8 的字体成立**。对 8×16 刚好精确;但如果哪天换了个 16 宽的字体,每行得取 2 字节,这个循环就得改。此刻我们不假装支持它——把边界划清楚,比留个隐患强。

## 调试现场

这一章没有专门的 notes 文件(`document/notes/013` 是空的),但代码里藏着三个真实的、写错就一定翻车的隐患,值得当成「调试现场」拎出来——它们都是你第一次写 framebuffer 几乎一定会撞上的。

**第一个,也是最关键的:帧缓冲通常在高地址,1GB 大页是「刚需」而不是「优化」。** QEMU 的 Bochs VBE 线性帧缓冲,物理地址常常落在几 GB 开外(远超 1GB)。这意味着 `map_mmio` 里第一段(<1GB 的 2MB 页)根本覆盖不到它,真正起作用的是第二段——1GB 大页。而 1GB 大页要 `has_1gb_pages()` 返回 true。如果你的代码漏了这一段、或者跑在一个不支持 PDPE1GB 的 CPU 上,显存就映射不进来,`Framebuffer::init` 之后第一个 `put_pixel` 立刻 page fault。所以记住:`map_mmio` 的两段不是「锦上添花」,而是覆盖不同物理地址区段的**两条腿**,少了高地址那条腿,屏幕就永远是黑的。

怎么确认你栽在这一步?最直接的办法是在 `Framebuffer::init` 调 `map_mmio` 之前,先把 `fb_addr` 用 kprintf 打出来(`fb_addr` 本身在 BootInfo 里,不依赖映射就能读)。如果它是个像 `0xe0000000` 这样的大数,你就知道一定得走 1GB 页那条路。然后确认 `map_mmio` 第二段的 `if (end > PAGE_1GB_SIZE && has_1gb_pages())` 真的进了、`pdpt[n]` 真的写进去了。`pdpt[n] == 0` 这个判断也别漏——它保证不覆盖已有映射,但反过来,如果你期望覆盖却没覆盖,就得查这个条件。

**第二个,硬编码的页表地址是这整套方案最脆的地方。** `PD_VIRT_ADDR = 0xFFFFFFFF80003000`、`PDPT_VIRT_ADDR = 0xFFFFFFFF80002000` 这两个常数,是「bootloader 把页表放在了这里」这个约定的硬编码。它对的时侯一切都好;一旦 bootloader 那边动了页表的布局(比如换了链接地址、加了新表),这两个地址就指向了别处,你往里写表项等于在破坏随机内存,症状是各种莫名其妙的花屏、崩溃、甚至 triple fault,而且没有任何报错告诉你「页表地址错了」。这种 bug 极难定位,因为代码看起来完全没毛病。我们这里的对策是**明知它是临时的**——等 015、016 做了正经的页表管理器,这种摸黑改表的做法会被替掉。在那之前,如果你动了 boot 的页表布局,第一件事就是回来核这两个地址。

**第三个,`pitch/4` 和「宽度 ≤ 8」这两个隐含假设。** `addr_[y * (pitch_/4) + x]`,少除了那个 4、或者误用 `width_`,画面就会整体歪斜错位(不是黑屏,是「能亮但全错」,反而更容易让人怀疑别的逻辑)。字体那边,如果哪天换了宽字体忘了改 `render_char` 的逐字节取位,字符就会只画出左半截。这两个都不是会崩的错,而是「安静地错」,排查时容易绕远路。把它们当成已知边界记在心里,撞上时能第一时间想到。

## 验证

这一章的验证分两层:能在 host 上跑的纯算术单测,和必须在 QEMU 里跑的真硬件测。

纯算术部分,host 单测把驱动里的关键公式**镜像**了一份出来测(注意:不是直接测内核代码,而是把同一个公式抄到测试里测,因为内核代码本身在 host 上跑不起来)。比如 [test_framebuffer.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_framebuffer.cpp) 测的就是那个下标公式和越界判断:

```cpp
static uint32_t pixel_index(uint32_t x, uint32_t y, uint32_t pitch) {
    return y * (pitch / 4) + x;     // 镜像 Framebuffer::put_pixel 的下标
}
TEST("framebuffer: pixel index at (100, 50)") {
    // pitch=4096(即 1024*4), offset = 50*(4096/4)+100 = 50*1024+100
    ASSERT_EQ(pixel_index(100, 50, 4096), 51200u + 100u);
}
```

同理 [test_font.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_font.cpp) 测 PSF2 header 解析和字形偏移,[test_console.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_console.cpp) 测光标/换行逻辑(那部分属于 013b,但算术单测放在同一批)。这些用 `-O2` 编、由 `CINUX_HOST_TEST` 宏门控,直接:

```bash
ctest --test-dir build -R 'font|framebuffer|console' --output-on-failure
```

但算术对,不代表真屏幕能亮——显存映射、VBE 模式这些只有 QEMU 里才验得了真。所以还有一组**机内测** [test_video.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_video.cpp),它在 QEMU 里跑,对着 bootloader 切出来的真 VBE framebuffer(1024×768×32)测:

```cpp
void test_fb_init_from_bootinfo() {
    auto* bi = reinterpret_cast<const BootInfo*>(BOOT_INFO_PHYS);
    g_fb.init(*bi);
    TEST_ASSERT_EQ(g_fb.width(), 1024u);
    TEST_ASSERT_EQ(g_fb.height(), 768u);
    TEST_ASSERT_GE(g_fb.pitch(), g_fb.width() * 4u);   // pitch >= width*4
    TEST_ASSERT_EQ(g_fb.pitch() % 4, 0u);              // 且 4 字节对齐
}
void test_fb_put_pixel_readback() {
    g_fb.clear(0);
    g_fb.put_pixel(100, 100, 0x00FF0000);
    TEST_ASSERT_EQ(g_fb.get_pixel(100, 100), 0x00FF0000u);  // 写进去读得回来
    TEST_ASSERT_EQ(g_fb.get_pixel(99, 100), 0u);            // 邻像素仍是黑
}
```

`get_pixel` 能把 `put_pixel` 写进去的值原样读回来,就证明那段显存确实被映射进来了、下标公式也对——这其实一并验证了 `map_mmio` 真的生效。跑它用带测试钩子的内核:

```bash
cmake --build build --target run-big-kernel-test
```

这组测试绿的,「显存能访问、像素能写能读、字体能画」就算点亮了。至于把这些像素组织成会换行、会滚动的文本终端,并让 kprintf 也往屏幕上吐——那是 013b 的活。

## 下一站

到这一步,内核已经能在屏幕上画像素、画字了。但你会注意到一个落差的:我们画字的能力(`PSFFont::render_char`)和内核唯一的诊断通道(kprintf)之间,还隔着一层——kprintf 此刻依旧只走串口,我们刚搭好的屏幕画字能力,还没有人调用它。

换句话说,这一章把「舞台」搭好了(像素、字体),但还没把「演员」(kprintf)请上来。下一站 013b 就做这件事:我们会在 framebuffer + 字体之上盖一层文本控制台 `Console`,管好光标、换行、滚动,然后回头兑现 012 那个一直悬着的承诺——把 kprintf 的格式化引擎接上屏幕这第二个输出后端,让每一句诊断同时出现在串口和屏幕上。012 当初抽出来的那个回调式架构,到那时才真正显示出它的价值。

---

### 参考

- OSDev — [VBE (VESA BIOS Extension)](https://wiki.osdev.org/VESA_Video_Standards):`INT 0x10` `AX=0x4F01` 取模式信息、`AX=0x4F02` 设模式、模式号 bit 14(`0x4000`)启用线性帧缓冲(linear framebuffer)。本章 bootloader 设模式的机制以此为准(注意 Cinux 是重定向到 0x144,非首创 VBE 调用)。
- OSDev — [PC Screen Font (PSF)](https://wiki.osdev.org/PC_Screen_Font):PSF2 魔数 `0x864AB572`、header 字段(magic/version/header_size/flags/length/charsize/height/width)、字形布局。
- Intel SDM Vol.3(System Programming,分页):4 级页表、2MB 大页(PDE.PS = bit 7)、1GB 大页(PDPE1GB,探测位 `CPUID.80000001h:EDX[26]`)、`INVLPG` 与 `MOV CR3` 的 TLB 刷新语义。本地 PDF `document/reference/intel/SDM-Vol3A-*.pdf`,可用 `pdf-reader` 搜 "PDPE1GB"/"PS (Page Size)" 复核位号。
- GCC 在线文档 — [`.incbin` 指令](https://sourceware.org/binutils/docs/as/Incbin.html):把二进制文件原样嵌入当前汇编段,导出起止符号。
- 本 tag 源码:[framebuffer.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/video/framebuffer.hpp) / [framebuffer.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/video/framebuffer.cpp)、[paging.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/paging.hpp) / [paging.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/paging.cpp)、[font.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/video/font.hpp) / [font.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/video/font.cpp) / [font_data.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/video/font_data.S)、[gen_psf_font.py](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/scripts/gen_psf_font.py)、[serial.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/common/serial.S)(VBE 模式重定向)、[boot_info.h](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/boot_info.h);测试 [test_framebuffer.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_framebuffer.cpp) / [test_font.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_font.cpp) / [test_video.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_video.cpp)。
