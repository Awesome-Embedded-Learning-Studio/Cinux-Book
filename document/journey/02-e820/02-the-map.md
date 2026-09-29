---
title: 02 · 把地图一次画全
description: "当年随手定的一个栈,当天夜里崩在一个 ret 上——布局的错不炸在布局处。这一遍把 boot 全生命周期的低内存一次画全,区间住户收拢成值类型,断言链替咱们看图。"
chapter: 2
order: 2
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - memory-map
  - layout
---

# 把地图一次画全

当时第一遍走到这里的时候,笔者给 stage2 挑栈,大概只花了十秒钟:`SS=0x0900`、`SP=0xFFFE`、能跑就行。到了当天晚上,加载内核的代码把内核安在了 `0x10000` 到 `0x90000`。这一带和栈的地盘叠了 36KB。用上一站教过的算术,咱们把这笔重叠算清:`SS=0x0900` 的段基是物理 `0x9000`,一个段可探的整整 64KB,所以栈的物理可达范围是 `0x9000` 到 `0x19000`,跟实际用了多深无关。它和加载区 `0x10000` 到 `0x90000` 的交叠恰好是 `0x10000` 到 `0x19000`,不多不少的 36KB。磁盘把内核读进来的那次写,把栈上还活着的返回地址盖了个干净。机器崩的时候,崩在了一个 `ret` 上——离读盘十万八千里,谁想得到病根是当天凌晨随手定的一个段值?布局的错从来不炸在布局那儿。它等代码长起来、等真正的住户搬进来,才在离现场最远的地方引爆。

> 这笔债当年还有下文:当天夜里笔者修栈、修加载地址,来来回回折腾了好几轮。病根笔者后来才看清:内存里谁住哪这件事,从头到尾没有一张图、没有一个管总的地方。每个数都散在各自的代码里——谁也不认识谁。

所以这一遍的头一件事,是把 boot 整个生命周期要用的低内存一次画全、一处定义:谁住哪、占多宽、挨着谁,全部写成了白纸黑字。将来要用、眼下还没用的,咱们也照样把地圈好——布局图的意义是防撞,而不是“现在用不用”。图就长在了 `boot/layout.hpp` 里:

![stage2 眼中的低内存地图](assets/stage2-memory-map.drawio)

页表区、VESA buffer、内核加载地址,其实眼下一个消费者都没有,可它们的宅基地今天就得圈好。您要是等真到要用的那天再画图,就是当年那个死法重演:新住户没地住,随手找块空地塞进去,塞进的恰好是别人家的客厅。占位区的尺寸也是按将来的消费者量的:页表区按三张 4K 的表,VESA 按两份要交给 BIOS 填的结构体——您学的是量法,不是什么两个数。

## 住户收拢成值

图上的住户,咱们按长相分了三种收法。

咱们把区间住户——自由区、页表区、VESA buffer——收进一个值类型 `MemoryRegion`,用的是半开区间 `[base, top)`,自带一个两家碰没碰上的判断。为什么要收拢?两个裸数 `base` 和 `top`,在函数参数的位置上想传反都没人拦:同是 `unsigned int`,谁是头谁是尾?类型系统一概不认。收进了一个类型,端点就没了交换的机会。而这个类型也没住在 boot,住进了 `base`——区间相交是跟“boot 还是内核”无关的数学件,将来的内存管理是它的重度用户。端点类型 `MemPtr_t` 用的是 `unsigned int` 不用 `unsigned short`,也是同一个理由:那边的地址不止 16 位。整份头文件是连一个 `#include` 都没有的,连 max、min 都是拿三目运算符手写的,不去碰 `<algorithm>`——那个头文件会一路 include 到 glibc 的系统头。依赖降到了零,它才有资格进 -m16 的世界。类型又是长了用户就难搬的家伙:眼下只有 boot 一家用它,咱们现在搬,成本不过三行。而每多一家用户,搬一次就多一份同步的活。

再看 stage2 自己的落点:段、偏移、扇区数,上一站在 `layout.hpp` 里还是三个散开的常量。本站咱们把它收成 `BootRegion` 一个值:

```cpp
struct BootRegion {
    unsigned short segments;
    unsigned short offset;
    unsigned short sectors;
};

inline constexpr BootRegion kStage2Spot{.segments = 0x0000, .offset = 0x7E00, .sectors = 8};
```

这玩意谁在用呢？MBR 那边 DAP 填它、`ljmp` 用它,构建的闸门也读它,三处认的是同一个名字。好在 `"i"` 约束可以接受成员访问。我们把 `kStage2Spot.offset` 递进去,反汇编里出来的还是上一站看过的五个字节，一个不差！栈的落点同理收成 `BootStack`,段加栈顶的两个字段。

## 断言链替咱们看图

图画好了,还得让编译器替咱们盯着它。`layout.hpp` 里排在常量后面的就是两串 `static_assert`。头一串管的是“有序”,咱们从上往下念:页表区不许低于自由区的底,不许越过 VESA buffer 的地界。VESA buffer 不许压着栈的地盘,栈不许探出自由区的边界。一条链从 `0x500` 一路排到 MBR 的门口——任何两个邻居贴得太近,构建当场就跟咱们翻脸。

```cpp
static_assert(kPageTables.base >= kLowFree.base);
static_assert(kPageTables.top <= kVesaBuffers.base);
static_assert(kVesaBuffers.top <= kStage2Stack.top);
static_assert(kStage2Stack.top <= kLowFree.top);
```

第二串管的是“语义”:stage2 的偏移必须正好是 MBR 加一扇区——紧贴是设计,而不是巧合。栈的段必须和 stage2 的段同值——毕竟同一个平坦世界。自由区的顶必须正好落在 MBR 的基上。页表区必须 4K 对齐——长模式的硬要求。您哪天改了图上一个数,编译器就按住所有跟它有关系的数、挨个对质。

其中自由区的顶等于 `kMbrBase` 的断言还真拦下过一回。图初画的时候,咱们照着社区资料和老代码抄了个 `0x7B00` 当自由区的顶,想的是“自由区顶到 MBR 前一扇区”。可您算算,咱们拿 `0x7C00` 减 `0x200`,得的是 `0x7A00`,而不是 `0x7B00`——值是抄来的,话是编的、俩对不上。人眼过多少遍都难抓的错——编译器一秒对质。`0x7B00` 的来历后来也查清了:当年第一遍的 DAP 参数包住在那儿,老资料顺手把它记成了自由区的顶。这一遍的 DAP 随 `.data` 住进镜像,那 256 字节没了住户——说得出住户的空隙才留在图上,说不出的、一律还给自由区。

## 当年那条公式的断言,提前进场

而最后一条断言,就是开篇那 36KB 的债换来的:stage2 在内存里的脚印,咱们按最坏情况算——8 扇区的预算窗口,加 E820 存档——不许越过内核加载地址。它住在了 `stage2.cpp` 里:

```cpp
static_assert(static_cast<unsigned long>(cinux::boot::kKernelLoadLma) >
                  static_cast<unsigned long>(cinux::boot::kStage2Spot.offset) +
                      (static_cast<unsigned long>(cinux::boot::kStage2Spot.sectors) * 512U) +
                      sizeof(cinux::boot::MemoryMap),
              "Guard: kernel load address overlaps stage2 footprint");
```

它为什么不住 `layout.hpp`?因为它要同时看得见布局常量和 E820 条目的大小,而 `layout.hpp` 想保持一张不依赖任何人的纯常量地图。断言长在使用现场的代码里,地图保持干净,两边也就各得其所了。当年那个公式本质上就是这么一句“栈也好加载区也好,加起来不许越过内核的家门”。只不过它是崩了之后才补上的,咱们今天提前把它在编译期确定下来。

地图确定下来了,咱们这就按图搬家——头一位:栈。
