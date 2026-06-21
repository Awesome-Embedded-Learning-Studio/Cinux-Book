---
title: 010 · 大内核的 GDT
---

# 010 · 大内核的 GDT:给内核铺好"段"的地基

> 本篇是 010 的上半篇。下半篇 [010b](010b-big-kernel-idt-exceptions.md) 讲 IDT 与异常处理。两篇合起来,就是 big kernel 从"只会打印"到"能扛住 CPU 异常"的完整一跳。

## 这一章我们要点亮什么

跑到这里,我们的 big kernel 已经能被 mini kernel 从磁盘加载、跳进来,还能用 `kprintf` 往串口吐字。但它有个尴尬的处境:**它脚下的"段"是别人铺的**。

mini kernel 为了把 big kernel 跑起来,顺手塞了一张临时 GDT 进去。那张 GDT 只够"别崩",谈不上结构。这一章,我们要让 big kernel 自己建一张正式的 GDT:7 个 entry,空段、内核代码、内核数据、用户代码、用户数据各占一个,再加上占两个槽的 TSS;然后用 `lgdt` 把它交给 CPU,再借一次远跳把 `CS` 切到我们自己写的内核代码段;等一切就位,`CS` 是 `0x08`、`DS/SS/ES` 是 `0x10`,段寄存器全部指向我们定义的段。

验证也很直接:`make run-big-kernel-test` 里有一组断言,直接读 `CS/DS/SS/ES` 的值,确认它们等于我们设的选择子。段铺没铺对,机器替你检查。

## 为什么现在需要它

很多人写到 long mode 会卡在一个问题上,我们先把它的皮扒掉——

**"段基址在 64 位下都被忽略了,GDT 不是该退休了吗?"**

没退休。long mode 忽略的是段描述符里的 base 和 limit(也就是段的起始地址和长度),但选择子(selector)这东西本身仍然必需,而且后面的几乎每一章都在踩它。

最直接的,`CS` 必须指向一个有效的、L 位为 1 的长模式代码段——CPU 虽然不再看 base,但它照样要检查这个描述符的属性位:是不是 present、是不是 code、是不是 64 位。这里给错了,收获的就是一个干脆利落的 #GP。再往下,特权级的切换全靠选择子最低两位的 RPL 和 CPL,后面我们要进 ring3 跑用户程序,本质就是把 `CS` 从 `0x08` 换成 `0x1B`,而能换的前提是 GDT 里得有对应的用户段。中断返回(IRETQ)也一样,它要恢复的 `CS`、`SS` 都是选择子,背后必须有有效描述符撑着。就连 TSS 也得挂在 GDT 里——任务切换、IST 中断栈切换,全靠 `TR` 指向 GDT 中的 TSS 描述符。

所以结论有点反直觉:long mode 不是不要 GDT,而是 GDT 的角色从"管内存分段"退化成了"管段属性、特权、TSS 的查表入口"。地基换了种铺法,但还是地基,这一章铺的就是它。

那 009 的时候为什么没崩?因为 mini kernel 留下的临时 GDT 恰好够 big kernel 用 `kprintf` 蹭两步。可一旦要碰特权级、碰中断、碰 TSS,那张临时表就不够看了——必须自己来。

> 外部依据:Intel SDM Vol.3A 在 Segment Descriptors 一节说明,long mode 下代码段描述符的 L 位为 1 时进入 64 位模式,此时 base/limit 被视为 0/无限。OSDev 的 Global Descriptor Table 页对"64 位下 GDT 的精简角色"有社区视角的总结。(精确章节号我们在写完后用本地 SDM 核实,见篇末参考。)

## 设计图

先把要建的 GDT 画出来。它是一段连续内存,每 8 字节一个 entry:

```text
偏移   entry            选择子    access   用途
0x00   [ null        ]    —        0x00    第 0 项必须全 0(CPU 规定)
0x08   [ kernel code ]    0x08     0x9A    内核代码段,L=1 长模式
0x10   [ kernel data ]    0x10     0x92    内核数据段
0x18   [ user   code ]    0x1B     0xFA    用户代码段(RPL=3)
0x20   [ user   data ]    0x23     0xF2    用户数据段(RPL=3)
0x28   [ TSS  low   ]  ┐  0x28     0x89    64 位 TSS,104 字节,
0x30   [ TSS  high  ]  ┘                    跨两个 8 字节槽
```

两个关键点先记牢:

- **选择子 = (entry 偏移) | RPL**。`0x08` = "第 1 项,RPL=0";`0x1B` = "第 3 项(偏移 0x18)再 `| 0x03`" = `0x1B`,RPL=3。选择子低 3 位里,bit 2 是 TI(0=查 GDT),bit 0-1 是 RPL。
- **TSS 占两个槽**。64 位 TSS 是 104 字节,一个 8 字节描述符装不下它的 64 位 base,所以用两个连续槽:一个装 limit + 低 32 位 base,一个装高 32 位 base。这就是为什么 entries 数组是 **7** 而不是 6。

再看一个 8 字节段描述符的内部结构:

```text
字节:    0        1        2        3        4        5        6        7
      ┌────────┬────────┬────────┬────────┬────────┬────────┬────────┬────────┐
      │ limit  │ base   │ base   │ access │ flags  │ base   │  (续)  │  (续)  │
      │  low   │  low   │  mid   │  byte  │+lim hi │  high  │        │        │
      │ (16b)  │ (16b)  │ (8b)   │ (8b)   │ (8b)   │ (8b)   │        │        │
      └────────┴────────┴────────┴────────┴────────┴────────┴────────┴────────┘
      access byte 各位(8 位): | P | DPL(2) | S | Type(4) |
      flags nibble(字节高 4 位): | G | D/B | L | AVL |
```

long mode 代码段的标志组合是 `access=0x9A`、`flags=0xA`(G=1, L=1)。下面我们把这几个数一行行算给你看,不是背下来的。

## 代码路线

源码主要在 [gdt.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/gdt.hpp) 和 [gdt.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/gdt.cpp)。从"怎么描述一个段"讲到"怎么把整张表加载进 CPU"。

### 1. 用 scoped enum 描述段属性,而不是裸位操作

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

### 2. 7 个 entry 怎么填

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

### 3. load():lgdt、远跳、ltr

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

## 验证

讲完了,得能跑出来。`make run-big-kernel-test` 会在 QEMU 里跑一组测试,其中四条直接读段寄存器:

```cpp
void test_cs_register() {
    uint16_t cs = 0;
    __asm__ volatile("movw %%cs, %0" : "=r"(cs));
    TEST_ASSERT_EQ(cs, GDT_KERNEL_CODE);   // 期望 0x08
}
```

`DS/SS/ES` 同理期望 `0x10`。要是 `lgdt` 之后忘了刷新 `CS`、或选择子算错位,这几条断言当场挂——比在真机上莫名其妙重启友好太多了。

手动看的话,`make run` 会打印:

```text
[BIG] Big kernel running @ 0x1000000
[BIG] GDT loaded.
```

看到 `GDT loaded.` 就说明 `init()` + `load()` 一路走通。

## 下一站

到这里,big kernel 脚下有了正经的段地基,`TR` 也挂上了 TSS。可你要是现在故意触发一个异常(比如 `int $3`),内核会直接三重故障重启——因为我们**还没有 IDT,没有任何异常兜底**。

这正是 [010b · IDT 与异常](010b-big-kernel-idt-exceptions.md) 要干的活:建 IDT、写 ISR、让 `int $3` 被接住、dump 出寄存器、然后活着继续跑。GDT 是地基,IDT 是安全网,两篇合起来,内核才第一次"扛得住事"。

---

### 参考

- Intel SDM Vol.3A — §3.4.5 Segment Descriptors(段描述符格式与位定义)、Long Mode 下的段角色、§8.7 + Figure 8-11(64 位 TSS 布局)。注:源码注释写的 "Table 8-2" 实为 32 位 TSS 的 Figure 8-2,64 位应为 Figure 8-11。
- OSDev — [Global Descriptor Table](https://wiki.osdev.org/Global_Descriptor_Table)、[Task State Segment](https://wiki.osdev.org/Task_State_Segment)。URL 有效性同样待核实。
- 本 tag 源码:[gdt.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/gdt.hpp)、[gdt.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/gdt.cpp);测试 [test_gdt_idt.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_gdt_idt.cpp)、[test_gdt_idt.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_gdt_idt.cpp)。
