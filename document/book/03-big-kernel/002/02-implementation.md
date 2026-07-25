---
title: 02 · 代码路线:scoped enum 与 7 个 entry
---

# 代码路线:scoped enum 与 7 个 entry

源码主要在 [gdt.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/gdt.hpp) 和 [gdt.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/gdt.cpp)。从"怎么描述一个段"讲到"怎么把整张表加载进 CPU"。

## 1. 用 scoped enum 描述段属性,而不是裸位操作

最朴素的写法是直接拿 `uint8_t` 拼位:写个 `0x9A` 代表内核代码段。能用,但有两个毛病——**写的人得记住每一位的含义,读的人更惨;而且拼错了编译器不会吱声**,要等到运行时某个莫名其妙的 #GP 才暴露。

Cinux 的选择是把每个属性位定义成强类型枚举(`scoped enum`),再用 `constexpr` 工厂函数拼成 entry:

```cpp
enum class SegmentAccess : uint8_t {
    Present    = 1u << 7,   // P 位:描述符是否有效
    Ring0      = 0u << 5,   // DPL = 00
    Ring3      = 3u << 5,   // DPL = 11
    CodeData   = 1u << 4,   // S 位:1=代码/数据段,0=系统段(如 TSS)
    Executable = 1u << 3,   // E:1=代码段
    ReadWrite  = 1u << 1,   // RW
    TSS64Avail = 0x09,      // 系统段类型:64 位可用 TSS
};

static constexpr Entry segment_entry(SegmentAccess access, SegmentFlags flags) {
    return {
        .limit_low        = 0xFFFF,
        .base_low         = 0,
        .base_middle      = 0,
        .access           = static_cast<uint8_t>(access),
        .flags_limit_high = static_cast<uint8_t>((static_cast<uint8_t>(flags) << 4) | 0x0F),
        .base_high        = 0,
    };
}
```

好处不是"好看",是**编译期可验证**。配合 `static_assert(sizeof(Entry) == 8)` 和 `static_assert(sizeof(TaskStateSegment) == 104)`,结构错位直接卡在编译期——这种错要是漏到运行时,你面对的会是一个极难定位的 #GP。

选择子也用常量,不写魔法数:

```cpp
constexpr uint16_t GDT_KERNEL_CODE = 0x08;
constexpr uint16_t GDT_KERNEL_DATA = 0x10;
constexpr uint16_t GDT_USER_CODE   = 0x1B;
constexpr uint16_t GDT_USER_DATA   = 0x23;
constexpr uint16_t GDT_TSS         = 0x28;
```

## 2. 7 个 entry 怎么填

[gdt.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/gdt.cpp) 的 `init()` 一口气把表填好。我们把内核代码段那行拎出来算一遍:

```cpp
entries_[1] = segment_entry(
    SegmentAccess::Present | SegmentAccess::Ring0 |
    SegmentAccess::CodeData | SegmentAccess::Executable | SegmentAccess::ReadWrite,
    SegmentFlags::Granularity4K | SegmentFlags::LongMode);
```

把 access 那串枚举 OR 起来:`0x80 | 0x00 | 0x10 | 0x08 | 0x02 = 0x9A`。
flags 是 `Granularity4K(0x08) | LongMode(0x02) = 0x0A`,放进字节高 4 位再 `| 0x0F`(limit 高 4 位全 1),得到 `0xAF`。

所以内核代码段描述符就是 `access=0x9A, flags_limit_high=0xAF`——和设计图对上了。其余几项同理,只换属性位。内核数据段把 Executable 去掉就是 `0x80 | 0x10 | 0x02 = 0x92`,flags 换成 `Granularity4K | Size32`;用户段把 Ring0 换成 Ring3,代码段得 `0xFA`、数据段得 `0xF2`。

> 顺手提一句数据段为什么用 `Size32` 而不是 `LongMode`:数据段的 D/B 位(对应这里的 Size32)决定默认操作数和栈指针大小,内核数据段设成 32 位兼容语义即可;而 L 位(对应 LongMode)只对代码段有意义,数据段不用碰它。

**TSS 是个特例**。它是系统段(S=0),而且 64 位 TSS 的 base 是 64 位的,一个 8 字节槽装不下,得拆两个:

```cpp
const auto tss_addr = reinterpret_cast<uint64_t>(&tss_);
entries_[5] = tss_low_entry(tss_addr, sizeof(TaskStateSegment) - 1);
entries_[6] = tss_high_entry(tss_addr);
```

`limit = sizeof(TSS) - 1 = 103`;`access = Present | TSS64Avail = 0x80 | 0x09 = 0x89`;高 32 位 base 进 `entries_[6]`。这就是 entry 数为 7、TSS 选择子是 `0x28` 而它后面紧跟一个 `0x30` 影子槽的原因。

> 为什么 TSS 恰好 104 字节?Intel SDM Vol.3A 的 64 位 TSS 布局图(Figure 8-11,64-Bit TSS Format)定义了它的字段:1 个保留 + 3 个 RSP(给 ring 0/1/2)+ 7 个 IST + I/O 位图基址等。顺带一提,源码注释里把它写成 "Table 8-2",但 8-2 其实是 32 位 TSS 的图(Figure 8-2),64 位 TSS 的正确编号是 Figure 8-11——这是写文档时拿本地 SDM 核实出来的一个源码注释笔误。

## 3. load():lgdt、远跳、ltr

表填好了,但 CPU 还不知道它在哪。`load()` 干三件事:

```cpp
gdtr_.limit = sizeof(entries_) - 1;   // GDTR:limit = 表长 - 1
gdtr_.base  = reinterpret_cast<uint64_t>(entries_);

__asm__ volatile(
    "lgdt %[gdtr]\n\t"            // ① 告诉 CPU:GDT 在这
    "pushq %[cs]\n\t"             // ② 把目标 CS 压栈
    "leaq 1f(%%rip), %%rax\n\t"   //    算出 "1:" 标号地址(之后的返回点)
    "pushq %%rax\n\t"            //    压栈作为返回 RIP
    "lretq\n\t"                   //    远返回:弹出 CS 和 RIP → CS 被刷新
    "1:\n\t"
    "movw %[ds], %%ax\n\t"        // ③ 刷新各数据段寄存器
    "movw %%ax, %%ds\n\t"
    /* ... es / fs / gs / ss 同理 ... */
    :
    : [gdtr] "m"(gdtr_), [cs] "i"(GDT_KERNEL_CODE), [ds] "i"(GDT_KERNEL_DATA)
    : "rax", "memory");
```

**为什么要用 `push cs + lretq` 这么绕的方式刷新 `CS`?** 因为 x86-64 根本没有 `mov cs, ...` 这条指令——`CS` 不能用 `mov` 改。能改 `CS` 的只有远跳/远返回一类(`ljmp`、`lret`)。所以这里的套路是:在栈上伪造一个"远返回现场"(压入目标 CS + 返回地址),再用 `lretq` 把它们弹进 `CS/RIP`,顺便跳到 `1:` 标号继续往下跑。这是"加载新 GDT 后刷新 CS"的标准姿势,绕不开。

数据段就省事了,`mov` 直接刷。最后挂上 TSS:

```cpp
const uint16_t tss_sel = GDT_TSS;
__asm__ volatile("ltr %[sel]\n\t" : : [sel] "r"(tss_sel) : "memory");
```

`ltr` 把 TSS 选择子装进 `TR`,从此 `TR` 指向我们的 TSS。下半篇讲异常时你会发现,IST(中断栈表)就藏在这个 TSS 里——这也是 TSS 必须现在就建好的原因。
