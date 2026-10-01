---
title: 03 · 位置是代码,数值是断言
description: "BitMask 立法:定义点只出现位置、从不出现数值;deposit 收的是帧号不是地址,脏低位在移位里就被丢弃;还有 unsigned long 在 32 位世界只有 4 字节、页表项却要 8 字节的那场暗亏。"
chapter: 5
order: 3
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - base
  - static-assert
---

# 位置是代码,数值是断言

咱们动笔填表之前还得置办一套词汇。页表项也好、一会儿要碰的 EFER 也罢,身上挂的全是“第几位”的事,而笔者在位常数的事上栽过一回跟头:想在第三位上放个 1 的时候,笔下写出来的却是 `0b0010`,放亮的其实是第二位。位置和数值之间隔着一道要心算的坎,而人脑做的这道翻译是会出错的,错了还不响,它长得比谁都像正经的代码。所以这一遍,咱们把位置和数值的翻译从人脑里收走、交给一条家法:**咱们只在定义点写位置,而数值只许出现在断言里**。

咱们把这句话落到代码上:住在 base 里的新头文件 `bit_ops/bitmask.hpp`,咱们挑出核心的几行放在下面。

```cpp
struct BitRange {
    unsigned char low;
    unsigned char width;
};

template <typename T>
struct BitMask {
    static_assert(T(-1) > T(0));

    T raw = 0;

    constexpr BitMask() = default;
    constexpr explicit BitMask(T value) : raw{value} {}

    [[nodiscard]] constexpr bool has(BitMask mask) const { return (raw & mask.raw) != T{0}; }

    constexpr BitMask operator|(BitMask other) const { return BitMask{raw | other.raw}; }

    [[nodiscard]] constexpr T extract(BitRange range) const {
        return static_cast<T>((raw >> range.low) & field_mask(range.width));
    }

    constexpr void deposit(BitRange range, T value) {
        T const kPlaced = static_cast<T>(field_mask(range.width) << range.low);
        raw             = static_cast<T>((raw & ~kPlaced) | ((value << range.low) & kPlaced));
    }

private:
    static constexpr T field_mask(unsigned char width) {
        return static_cast<T>((T{1} << width) - T{1});
    }
};

template <typename T>
constexpr BitMask<T> MaskBit(unsigned char position) {
    return BitMask<T>{static_cast<T>(T{1} << position)};
}
```

咱们把词汇过一遍。`MaskBit(7)` 说的是“第 7 位”,它算出来的 `0x80`,您在定义点里是搜不到的,因为定义点只写了 7。`BitRange{.low = 21, .width = 31}` 说的是“从第 21 位起、宽 31 位的一段”,而指定初始化让 low 和 width 各归各的名字,想把它们传反都没机会了。而 `deposit` 负责往一段里放值、`extract` 负责把值读回来,是一对互逆的操作。`has` 问的是某几位亮没亮。开头的那行 `static_assert(T(-1) > T(0))`,用零依赖的写法把“只收无符号”定了下来——不拉 `<type_traits>` 的理由,跟咱们画低内存地图时给 MemoryRegion 头文件立的零 include 家法是同一条,咱们要这个文件在每一个编译世界里都编得过。

这套词汇跟咱们用熟的 `[[gnu::packed]]` 结构体怎么分工?咱们把界线划清楚:字节对齐的硬件结构,像 DAP、E820 的条目、GDT 的描述符,继续走 packed 结构体的路,因为字段落的本来就是字节边界。而子字节的位段,从本卷起就归 BitMask 管了,两套各管各的场景、谁也不换掉谁。笔者还把克制一并立了字据:不上 tag、不上宏、不上迭代器,它就是一个带着词汇的整数。而将来内存管理要用它的时候,咱们只往里添方法、不推倒重来。

宽度本身就是这套词汇里最值钱的一件事,咱们多看一眼。`BitMask<T>` 的 T 是字类型:一面 32 位寄存器的旗,想或进一个 64 位的页表项,`operator|` 两边的 T 就对不上了,编译期当场就炸了——宽度成了类型系统里的事实,不靠人的自觉。这套护栏上岗头一天拦下的头一个,就是咱们自己。笔者写三个旗常量的时候,手一顺把模板参数写成了页表项自己的别名,而编译器报回来的类型,是 `BitMask<BitMask<…>>` 套了两层。而它连自己人都不认,当然也就谁都不认了,这正是咱们花钱买它来的脾气。

词汇备齐了,咱们把页表项的语法立起来。它住在 base 的另一个新头文件 `page/page_entry.hpp` 里:

```cpp
using Entry = bit::BitMask<uint64_t>;

inline constexpr Entry kPresent = bit::MaskBit<uint64_t>(0);
inline constexpr Entry kWritable = bit::MaskBit<uint64_t>(1);
inline constexpr Entry kLarge = bit::MaskBit<uint64_t>(7);

inline constexpr unsigned char kLargePageShift = 21;
inline constexpr uint64_t kLargePageSize = 1ULL << kLargePageShift;

inline constexpr bit::BitRange kTablePhys{.low = 12, .width = 40};
inline constexpr bit::BitRange kLargePagePhys{.low = 21, .width = 31};

constexpr Entry MakeTableEntry(uint64_t physical, Entry flags) {
    Entry entry{kPresent | flags};
    entry.deposit(kTablePhys, physical >> kTablePhys.low);
    return entry;
}

constexpr Entry MakeLargePageEntry(uint64_t physical, Entry flags) {
    Entry entry{kPresent | kLarge | flags};
    entry.deposit(kLargePagePhys, physical >> kLargePagePhys.low);
    return entry;
}
```

您注意工厂里 deposit 之前的那次右移,`physical >> kTablePhys.low` 和 `physical >> kLargePagePhys.low` 干的都是同一件事:deposit 收的,是字段值而不是地址本体。表项的地址字段里存的是帧号:指针项存的是“下一张表的地址右移 12 位”的商,而大页项存的是“页基址右移 21 位”的商。这道翻译咱们只在工厂里做一次,而移位量跟 BitRange 的 low 是同一个常量,所以一个魔法数都不用写。翻译还附赠一层白净:地址上的脏低位,会被右移整个地丢掉。咱们拿 `MakeLargePageEntry(0x200123, kWritable)` 真算一遍。咱们把 `0x200123` 右移 21 位,得到的商是 1,低 21 位那截 `0x123` 就被挤掉了。deposit 把 1 放进从 21 位起的字段,出来的就是分毫不差的 `0x200083`。咱们头一版草图就把地址整个喂给了 deposit,而那样会移位两次、一错就是一整个页。如今这样的语义有哨兵守着,谁再犯了,编译期当场就拦下了:

```cpp
static_assert(sizeof(Entry) == 8);
static_assert(kLargePageSize == 0x200000);
static_assert(MakeTableEntry(0x2000, kWritable).raw == 0x2003);
static_assert(MakeTableEntry(0x2007, kWritable).raw == 0x2003);
static_assert(MakeLargePageEntry(0x0, kWritable).raw == 0x83);
static_assert(MakeLargePageEntry(kLargePageSize, kWritable).raw == 0x200083);
static_assert(MakeLargePageEntry(0x200123, kWritable).raw == 0x200083);
```

`0x2007` 的断言,跟 `0x200123` 是同一个意思的指针版:对齐好的表地址进来,尾巴上没扫干净的位,在进项之前就一律被裁掉了。咱们让模板算一遍、手写的值再算一遍,两边的值要是对不上,编译期就过不去了——这套手法咱们从 GDT 那排断言起就没换过。

末了的那句 `Entry = BitMask<uint64_t>`,它是本卷的立法句,咱们单独说。咱们为什么把类型写成 `uint64_t`,而不写平时顺手的 `unsigned long`?因为宽度是编译世界的属性,而类型名给的承诺,其实是靠不住的:`unsigned long` 在 32 位的世界里只有 4 个字节,而页表项要的是 8 个字节。这不是咱们脑补的败局,动工前的实地探测里,它真的炸过一回。拿 4 字节的类型去填 8 字节的项,而 C++ 这边写得自洽,MMU 那边却是拿 8 字节当一项读的,相邻的两颗就被它拼成了一项。咱们在 GDB 里 `x/8gx` 一看,本该一项一格的表里,两个半截挤在一个 64 位的格子里。更阴的是 host 那头:LP64 的 `unsigned long` 恰好 8 个字节,单测在那边就绿着放行了——单测的护栏有它的边界,而类型把宽度定住的护栏,恰好是从边界外接手的。

所以这里一共设了三道关。类型咱们直接写死成 `uint64_t`,宽度就不再跟着编译世界漂了,这是头一道的关。第二道是 `sizeof(Entry) == 8` 的断言,它跟着头文件走进每一个编译世界:如今它在四个世界里是编得过的,可谁要是把类型换回了 `unsigned long`,那个版本一进 -m32 的世界就会被断言当场拦下。第三道是头文件里写的就是 `<stdint.h>`,而 `<cstdint>` 咱们在前几卷验过,在这儿是拉不开的。

> 而这个头文件不住 boot,而是住进了 base。咱们有意把页表项的语法从引导代码里分出来:将来内核的内存管理要做页表,直接就能把它们用上、不必再立第二套写法了。
