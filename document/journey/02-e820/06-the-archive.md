---
title: 06 · 二十四字节,一条一条确定下来
description: "24 字节 packed 条目、跟着结构住进头文件的十六条断言、raw type 的保守翻译,以及 16 位世界里打 64 位数的两件麻烦。"
chapter: 2
order: 6
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - e820
  - memory-map
---

# 二十四字节,一条一条确定下来

图谱有了住处:`stage2.cpp` 里一个安安静静的全局变量 `g_memory_map`。它没带初值就落在了 `.bss`。上一站咱们立过一条纪律:16 位世界里 BIOS 不清内存,`.bss` 的全局变量醒来是什么值全看上辈子,所以家当一律带初值躺 `.data`。立的纪律在这儿翻了个面——`g_memory_map` 照样住 `.bss`,照样可能醒来一脑子的垃圾,不过没关系,咱们不假设它是零:`count` 由 `CollectMemoryMap` 头一行亲手归了零,条目区只有 BIOS 写完之后才有读者。纪律倒是没破:没有谁赶在 BIOS 写完之前去读它。

这套手写的纪律咱们走成了一遍动画:BIOS 把条目一条一条写进来,写完之后读者才进门。您看的时候,数一数 count 攒到了第几条:

<Anim id="e820-archive-seven" />

## 一条的身材:二十四字节

```cpp
struct [[gnu::packed]] MemoryMapEntry {
    unsigned long long base;
    unsigned long long length;
    unsigned int       type;       // Raw types of MapEntry
    unsigned int       acpi_attr;  // ACPI 3.0
};
```

BIOS 亲手往咱们递的房子里写的就是这副身材:base 八字节、length 八字节、type 四字节,三样凑成了老口径的 20 字节。末尾的 `acpi_attr` 四字节是 ACPI 3.0 扩展出来的,它的 bit 0 还兼着“作不作数”的开关,本站咱们不消费它,留的是字段,不留的是逻辑。您可能会问:BIOS 明明允许只写 20,咱们为什么备 24 的房子?两个理由。头一个是对齐的考虑:条目要是 20 字节一条,数组里第二条的 base 就落不到 8 的倍数地址上,64 位的字段全体别扭,而 24 是 8 的倍数,一条一条地排下去,家家都对齐了。第二个是口径:咱们递进去的宽度就是 24,碰上支持 ACPI 3.0 的 BIOS 就写满。不支持的写 20,尾巴上留着的是 `.bss` 醒来时的旧值——好在本站没人读它。房子备的是 24,BIOS 只写了 20 也装得下,咱们不亏。

咱们又见面了,`[[gnu::packed]]`!不过这回它的身份有点微妙:四个字段天然就排得紧凑:base 在 0、length 在 8、type 在 16、acpi_attr 在 20,packed 实际上什么都没改。那还挂它干什么呢?

它干的事是把“紧凑”从碰巧升格成了合同。谁哪天往结构里加一个字段、谁的编译器动了垫空隙的念头,空隙就都别想进来了。DAP 那十六个字节是靠 packed 从垫空隙的边缘救回来的。二十四个字节这边则是提前上了门闩。同一个手段咱们出手了两次。

## 断言,住进声明现场

```cpp
static_assert(sizeof(MemoryMapEntry) == 24);
static_assert(__builtin_offsetof(MemoryMapEntry, base) == 0);
static_assert(__builtin_offsetof(MemoryMapEntry, length) == 8);
static_assert(__builtin_offsetof(MemoryMapEntry, type) == 16);
static_assert(__builtin_offsetof(MemoryMapEntry, acpi_attr) == 20);
```

五条断言跟结构住进了同一个头文件 `boot/e820/e820.hpp`,改结构的人第一眼就会看见它们。画布局图的时候说过“断言长在使用现场”,到这儿是同一个手法按了五遍。存档本体 `MemoryMap` 装的是 32 个条目加一个 `count`,咱们也给它的 sizeof 立了一条断言:32 乘 24 加 4,得的是 772 字节,含糊不了一个字节。`__builtin_offsetof` 的写法值得看一眼:标准库的 `offsetof` 住在 `<cstddef>` 里,那扇门在 -m16 的世界拉不开,libstdc++ 的包装会一路摸到 glibc 的系统头。GCC 的内建版本零依赖,效果是一模一样的。咱们顺着这扇门,把上一站立的“零标准头”说得更准:`e820.hpp` 自己就写了 `#include <stdint.h>`,`EntryType` 的底层类型 `uint8_t` 就从它来。这不算破例——边界本身定得更细了。GCC 自带的 C 头在 freestanding 下走自家的 `stdint-gcc.h`,连 glibc 的依赖都是零,所以这一层是放行的。真正禁的是 libstdc++ 那层 C++ 包装,`<cstdint>` 一进门就去摸 glibc 的系统头。

映射那边咱们还确定了十条,住法是相同的。`ClassifyEntry` 和 `IsUsable` 是 constexpr 的函数,常量求值的结果自己就能进断言:七条确定分类,1 到 5 的每个值一条,外加 0 和 `0xFFFFFFFF` 两个越界值也走保守的分支。三条确定 `IsUsable` 的口径:只有 kUsable 算可用——

```cpp
static_assert(ClassifyEntry(1) == EntryType::kUsable);
static_assert(ClassifyEntry(2) == EntryType::kReserved);
static_assert(ClassifyEntry(3) == EntryType::kAcpiReclaimable);
static_assert(ClassifyEntry(4) == EntryType::kAcpiNvs);
static_assert(ClassifyEntry(5) == EntryType::kBad);
static_assert(ClassifyEntry(0) == EntryType::kReserved);
static_assert(ClassifyEntry(0xFFFFFFFFU) == EntryType::kReserved);
static_assert(IsUsable(EntryType::kUsable));
static_assert(!IsUsable(EntryType::kReserved));
static_assert(!IsUsable(EntryType::kAcpiReclaimable));
```

十六条断言全跟结构住在了 `e820.hpp` 里。它们和上面的那串一样全是常量表达式,检查全部落在了编译期。编译期查布局的妙处在双世界:这个头既被 boot 用 -m16 的口径编,又被测试流水线用 -m64 的口径编,同一串断言在两个 ABI 的语境里各站一班岗。这班岗可不是白站的,它倒是真抓过东西,咱们马上就讲。

## raw type:原话照存,翻译保守

条目里的 type 字段,BIOS 写的是 1 到 5 这些裸值。咱们认得:1 是可用、2 是保留、3 是 ACPI 可回收。后两样咱们也认得:4 是 ACPI 非易失、5 是坏的内存。咱们的结构原样存它,翻译成枚举的事不急。为什么?BIOS 的值域不由咱们做主:约定自己都叮嘱“没列出来的地方当保留对待”,今天的 BIOS 完全可能写出个名单外的值来。可要是咱们在结构里直接放枚举,名单外的值往哪儿搁?枚举的名单是咱们定的,BIOS 的值域不是——硬收进枚举,值是装下了,可它到底什么意思,偏偏没人答得上。所以结构里存 raw 值,翻译推迟到读取的那一刻,交给了一个纯函数:

```cpp
constexpr EntryType ClassifyEntry(unsigned int raw_type) {
    switch (raw_type) {
    case 1: return EntryType::kUsable;
    case 3: return EntryType::kAcpiReclaimable;
    case 4: return EntryType::kAcpiNvs;
    case 5: return EntryType::kBad;
    case 2:
    default: return EntryType::kReserved;
    }
}
```

您品那个 `default`:名单之外的值、连同 0、连同 `0xFFFFFFFF`,统统归入了 kReserved。这个方向是有讲究的。把可用错当成了保留,损失的是几页内存,心疼一下就过去了。把保留错当成了可用,分出去的页踩在硬件洞上、ACPI 表上,崩起来可是没有现场的。将来管物理页的那几站拿到的家底,从探测这一侧就把底线立好了。

还有一处克制:收工序列里您会亲眼验证,咱们 dump 图谱时打出来的 type 是裸值,不是 ClassifyEntry 的翻译。探测层忠于 BIOS 的原话,解释的活留给消费层。

## 双世界纪律:宽度会漂移的类型,禁入

断言的岗哨真抓到过东西。咱们给结构定的第一版,32 位的字段图省事写成了 `unsigned long`。在 boot 这边咱们用 -m16 编,落进的是 32 位 ABI,`unsigned long` 恰好是 4 字节的个头,一切都太平了。同一份头拿到 host 那边——host 就是咱们一路说的您开发用的电脑,跑测试程序的那个世界——-m64 的 LP64 语境里它却是 8 字节,`sizeof(MemoryMapEntry)` 当场变成了 32,断言立马就响了,错误信息把 32 不等于 24 摆在了咱们脸上。改法只动了一行:32 位字段一律 `unsigned int`,它在两个世界都是同样的 4 字节,宽度写进了字面。这件事最后立成了一条纪律:跨世界共享的结构,字段只用两个 ABI 下同宽的内建。`unsigned int`、`unsigned short`、`unsigned long long` 都是合格的。宽度会随世界漂移的 `unsigned long`,咱们一个都不许进。执法的正是头里那套 sizeof 断言——可执法者要上岗,前提是两个世界真的都在编它。您想亲眼看这场对质也容易:把 type 和 acpi_attr 两个 32 位字段都改回 `unsigned long`,照着结构的第一版来,咱们再构建一次 test_host,断言当场就炸了,错误信息还是咱们头一回遇上的那句 32 不等于 24。

## 16 位世界里打 64 位数:两件麻烦

咱们要打印图谱,base 和 length 都是 64 位的大数,要在 16 位世界里把它们拆成一位一位的十六进制字符。咱们这一拆,当天就挖出了一整条矿脉。咱们摘两条大的讲,细节留给后面的概念篇——主线之外专讲概念、不搬代码的文章——慢慢展开。

麻烦一:变参在 -m16 断了。咱们的格式引擎是 base 库的当家件,host 上用的是变参入口 `VformatToBuf`。咱们把它搬进 16 位世界后,链接是绿的,一跑就飞了。反汇编一查抓到了真凶:同一个编译器、同一套旗子,调用方把变参压成 16 位的槽,被调方按 32 位的槽取,两端各说各的话。链接器对调用约定是一无所知的,链接是绿的,可不等于能跑的保证。解法是给引擎添一个类型擦除数组的入口 `FormatToBuf`:参数包成 `Arg` 结构体数组递过去,普通结构体走正常的调用约定,谁的槽多宽,合同里写得明明白白的。您要是觉得这路数眼熟——`std::format` 抛弃 printf 的变参,走的就是同一条路。

麻烦二:除法没有现成的轮子。咱们打印十进制要除以 10,64 位的大数在 16 位代码里做除法,GCC 会生成对 `__udivmoddi4` 的调用,那是 libgcc 的运行时件。链它吗?咱们这头是 16 位模式,链接进来的 `.o` 却是按 32 位模式语义预编译的机器码,咱们一头 call 进去,同一串字节全按 32 位的语义解码,压栈压错了宽度,ret 返错了长度,跑飞是没商量的。不链吗?链接期直接报未定义符号。解法朴素得可爱:自己写一份 `__udivmoddi4` 的 C 源码,64 轮的移位减法放进 `boot/early/libc.cpp`,用 -m16 的口径编出来,它天然就是 16 位语义的机器码。符号名双下划线是 libgcc 的 ABI 合同,GCC 生成的调用就认这个名字,咱们照名接单。`libc.cpp` 里还有一个作伴的 `strlen`,它俩算的是同一层:“16 位世界的运行时支撑”,base 引擎缺什么,这边补什么。
