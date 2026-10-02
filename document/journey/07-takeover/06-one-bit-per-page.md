---
title: 06 · 一页一 bit 的名册
description: "kernel/mm 开张,单子上的 E820 图谱头一回被内核侧真正消费:整本位图起手全记有主,usable 的页逐条放行,低 1MiB 一刀整块圈起,内核自己登记在册。半兆的名册住 .bss,管住十六个 G,盘上一个字节不为它花。"
chapter: 7
order: 6
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - memory
  - pmm
---

# 一页一 bit 的名册

嗓子的事办完了,咱们上第二道菜。上一卷内核打的第二行字,报的是“8 e820 entries”——单子上的图谱,boot 侧在九判里帮咱们问过一回,内核侧却只数了数条数、打了一行字,图谱本身的用处,其实一直没派上用场。图谱就这么躺在单子里,记着机器每一寸内存的身世:哪儿能用、哪儿是固件的地盘、哪儿是洞。这么一份家底,咱们总不能一直供着。本卷给它配上真正的用处:内核要造一本册子,把物理内存一页一页地登记下来——哪些页能分、哪些页已经有了主,咱们全都得记上一笔。这本册子有自己的正名:物理内存管理,平时咱们喊它 PMM。

PMM 的家,住在咱们本卷新开的第二个器官目录:`kernel/mm/`。册子的形态,咱们定成一页记一个 bit。页是多大?答案是 4KiB——这个数,住在一枚很小的头文件里:

```cpp
namespace cinux::arch::page {

using cinux::base::operator""_KiB;

/** @brief Page size of the x86-64 long mode this tree targets. */
inline constexpr unsigned long kSize = 4_KiB;

}  // namespace cinux::arch::page
```

您看它住的位置:kernel/arch/,而不住在 base。这是有意的:base 是与操作系统无关的家底,数学、位操作、地址类型,您搁哪个 OS 里都使得。而“一页多大”是机器的事,x86-64 的长模式说 4KiB,它就是 4KiB——架构的事实归 arch,这是咱们立下的分工。您再看 `4_KiB` 这个写法,它本身也是新添的语义件。base 备了几个字节单位的字面量:_KiB、_MiB、_GiB。咱们写尺寸再也不用裸乘 1024,乘法只准在它自家的头文件里出现。

册子的骨架是位图,这个数据结构是 base 这一卷新添的家当:管单个位的词汇 BitMask,大伙在长模式那一卷就用上了,把它排成一片、管住一整段区间的 Bitmap,是新垒上去的那一层。本卷咱们把它接进家谱。它有个很讲究的脾性:位图自己是不拥有存储的。字数组是主人的,位图只管借来登记——Pmm 的怀里抱着 `words_` 数组,初始化的时候递给 Bitmap 绑上。这么一来 base 一个字节都不用分配,而册子要占多少字,编译期就算平了:

```cpp
static constexpr unsigned long words_for(unsigned long bit_capacity) {
    return (bit_capacity + 63) / 64;
}
```

所以册子要多大,咱们把顶定在了 16GiB:顶以下的每一页发一个 bit。您拿 16GiB 除以 4KiB 在计算器上按按,是实打实的 4194304 页。每页记的只是一个 bit,摊下来的总数正好是 512KiB,合 65536 个 64 位的字。用半兆的册子管住十六个 G,一页 4KiB 的身家记在一个 bit 上,三万两千多倍的杠杆——这就是位图形态的理由。

造册的流程,咱们把 `pmm.cpp` 的 init 摆开看:

```cpp
bitmap_.init(words_, kPmmTotalPages);
bitmap_.set_all();
managed_pages_ = 0;
for (uint32_t index = 0; index < count; ++index) {
    if (entries[index].type != cinux::boot::kE820Usable) {
        continue;
    }
    auto const kEntryBase = static_cast<unsigned long>(entries[index].base);
    auto const kEntryTop  = kEntryBase + static_cast<unsigned long>(entries[index].length);
    auto const kLo        = kEntryBase > kLowMemoryTop ? kEntryBase : kLowMemoryTop;
    auto const kHi        = kEntryTop < kPmmMaxPhys ? kEntryTop : kPmmMaxPhys;
    if (kLo >= kHi) {
        continue;
    }
    unsigned long const kPageCount =
        cinux::base::math::Span(kLo, kHi, cinux::arch::page::kSize);
    bitmap_.clear_range(cinux::base::math::Floor(kLo, cinux::arch::page::kSize), kPageCount);
    managed_pages_ += kPageCount;
}
```

咱们头一笔就是 set_all——整本册子一律当成不空的,每一位都记成有主的。然后才一条一条地放行:图谱里 type 为 usable 的条目,两头一夹——下限是 1MiB 的红线,上限是 16GiB 的顶。落在这个窗口里的页,一页一页地清零,记成了可用。您注意登记的方向:从“谁都不许动”的状态起步,再一块一块地放行。方向反过来的册子,写错一笔就是凭空多出了一页能分给别人的内存。而按咱们这个方向写,错了顶多是把还能用的页当成了有主,咱们少用一点内存,是绝不会用错内存的。做册子咱们宁可保守一点。

那个 1MiB 的下限,咱们得单独说。它的名字叫 kLowMemoryTop,一刀切下去把低 1MiB 整块圈了起来——一位也不放行。凭什么?您把这 1MiB 想想看谁住着:MBR、stage2 本体、两代栈、三张表、摆渡的暂存窗、固件的数据区,boot 一路的脚印一个不落地全踩在这 1MiB 里——上一卷装载之前,九判拿四块脚印问的是“鞋盒跟它们打不打架”。如今内核进了场,同一份脚印换了个记法:不逐条重演那四块了,一刀下去把整块都圈了进去。装载前问的是“能不能住”,册子里记的是“谁已经住了”。这是同一份脚印的两个视角。诚实地讲,切得比四块脚印粗:低 1MiB 里其实还有零星的空地,这一遍咱们不捡,boot 的地界,就整块留给 boot 了。

循环转完了,咱们还差最后一笔要记:内核自己。init 的尾巴上,咱们把单子里的 kernel_paddr 和 kernel_mem_size 取出来,从内核的家到内核的尾巴,一页一页地 set 回去,登记成了已占。这俩字段上一卷内核只是拿出来打了行字核对,这一回真金白银地花上了——单子上的字段,这已经是第二次消费了。

咱们再算算册子本体住哪儿。它连字带壳是内核里的一个全局,整本躺在了 .bss 里。您回想上一卷的说法:盘上不掏钱、内存里占地的院子,内核自家的院子自己扫。院子还是上一卷的那座院子,只是这回院子里躺了一本 512KiB 的大名册。所以面板上内核的 size 才从上一卷的 C40 涨到了 810C0:盘上的内核 3740 个字节,内存里 528576 个字节的地界,差值几乎全是册子的分量。盘上为它花的字节——一个都没有,这是 BSS 的老红利,头一回被咱们用到这个量级。

册子还有一处设计的门道,咱们拿数据一对就明白:册子的尺寸,跟的是天花板,不是机器实际插了多少内存。咱们现在肯管到 16GiB,位就铺满到 16GiB 的顶。您看 QEMU 只给了 128MiB,铺出去的位绝大多数永远记着“不空”,一位也不骗您。哪台机器真插过了 16GiB,超出的部分,册子是明说不管的:顶就是顶,咱们不装无限。咱们在考古箱里还能翻出一笔对照,挺有意思的:当年第一遍的位图数组有多大,是拿链接器符号问出来的——链接脚本两头立界碑,代码运行的时候拿名字去问“我有多大”。而这一遍,页数从 16GiB 的顶直接推出来,是编译期的常量,字数也是编译期就算平的,链接器一页都不用答了——册子铺多大的决定权,在咱们定的管理上限手里,不在链接器摆段的巧合里。
