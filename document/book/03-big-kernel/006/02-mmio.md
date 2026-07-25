---
title: 02 · VBE 模式与 map_mmio:让显存能访问
---

# VBE 模式与 map_mmio:让显存能访问

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

所以画像素之前,必须先把 [paging.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/paging.cpp) 里 `map_mmio` 的活:

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
