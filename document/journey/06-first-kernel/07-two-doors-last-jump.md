---
title: 07 · 两扇门,和最后一跳
description: "三张表从临时桥长成交接门:PML4 的第 0 项与第 511 项指着同一张表,高半别名恒等于物理地址;门数按内核的尾巴算;末了入口坐进 rax、单子坐进 rdi,一条 jmp 递出执行流。"
chapter: 6
order: 7
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - paging
  - long-mode
  - kernel
---

# 两扇门,和最后一跳

内核的代码,取址全按高半的世界观来,0xFFFFFFFF80200000 就是它认的家。可 CPU 找指令走的是页表,上一卷铺的三张表,干的是物理地址原样落回原处的恒等映射——高处的世界里没有路,内核搬进去的当天,连自己的头一行代码都取不回来。所以三张表在本站换了个身份:上一卷它们是桥,把执行流送过河就算完了。这一卷该它们当门了。改动的活落在 `boot/lm/page_tables.cpp`,函数的名字也换成了 BuildHandoffDoors,咱们把核心摆出来:

```cpp
unsigned long const kCovered   = g_kernel_end_paddr + 0x1FFFFFUL;
auto const          kDoorCount = static_cast<unsigned int>(kCovered >> 21);
for (unsigned int i = 0; i < kDoorCount; ++i) {
    kPd[i] = MakeLargePageEntry(i * kLargePageSize, kWritable);
}

kPdpt[0]   = MakeTableEntry(kPdPhys, kWritable);
kPdpt[510] = MakeTableEntry(kPdPhys, kWritable);
kPml4[0]   = MakeTableEntry(kPdptPhys, kWritable);
kPml4[511] = MakeTableEntry(kPdptPhys, kWritable);
```

大页还是 2MB 的老相识,可张数是活的。上一卷铺的是固定四张,算的是过桥那一拍的低区。这一卷内核的大小住进了鞋盒,门就按尾巴开:g_kernel_end_paddr 是装载时记下的物理尾,咱们给它加 0x1FFFFF 再右移 21 位,换来的就是把尾巴按 2MB 上取整。咱们这版内核尾巴在 0x200C40,取整到了 4MB,两张 2MB 的大页:PD[0] 盖 0 到 2MB,PD[1] 盖 2 到 4MB——内核的家、单子、邮箱、三张表自己,全在头两页的门里。

真正的正主是末尾四行:PDPT 的第 0 项和第 510 项,指向的是同一张 PD。PML4 的第 0 项和第 511 项,指向的是同一张 PDPT。一张表挂了两个门牌。凭什么偏偏是这几号?咱们拿内核入口的高半门牌 0xFFFFFFFF802001B4,照上一卷走表的法子真走一遍——高半的门牌高 32 位全是 1,所以每一层都得老老实实按 9 位取:

```text
(0xFFFFFFFF802001B4 >> 39) & 0x1FF = 511  → 查 PML4[511]  → 指向 0x2000 的 PDPT
(0xFFFFFFFF802001B4 >> 30) & 0x1FF = 510  → 查 PDPT[510]  → 指向 0x3000 的 PD
(0xFFFFFFFF802001B4 >> 21) & 0x1FF = 1    → 查 PD[1]      → 大页,基址 0x200000
低 21 位是 0x1B4                            → 页内偏移
0x200000 + 0x1B4 = 0x2001B4                → 物理的家
```

咱们再拿同一个物理地址 0x2001B4 走低门:右移 39 位是 0、右移 30 位也是 0,进的是 PML4[0]、PDPT[0],到的还是同一张 PD 的同一颗 PD[1]。门牌发了两个,可走的路是同一条,到的家是同一个——高半的别名,恒等于物理地址。layout 里立的 kHighHalfBase 0xFFFFFFFF80000000,和链接脚本起手的那行世界观,是同一笔数目的两头。世界观在 0xFFFFFFFF80000000 上加了 2MB 的家,门早就开在那儿了。地址里加多少,门认的就是多少。

低门为什么留着不撤?咱们回想一下过桥那一拍:执行流还在低区,表自己、邮箱、单子——如今住的地界也都在低处。低门要是关了,PG 亮起的那一拍就断了路——恒等映射当年是为了过桥,如今是常驻的门:桥是一次性的,门是天天要开的。

门的限度也得交代清楚:门数按尾巴开,可门牌只发到一张 PD 的 512 颗——一颗 2MB,一张 PD 管的是 1GB。鞋盒要是把家报到 1GB 以上的高处,九判倒是放行,摆渡的顶也是 4GB,门偏偏开不到那儿:CPU 一进门,前头没了路,等着的还是三重故障——连句遗言都没有。咱们把这道天花板交代在这儿,不为难自己:内核的家安在 2MB,离顶还有老远的路。真到了往高处大举搬家的那天,咱们得再添表页,那是往后内存管理当家的活,门到那时自然是要扩的。

门开好了,该走最后一跳了。lm 世界打完了 `[lm] 64-bit world alive` 那行字,做它在人间的最后几件事,咱们把 `boot/lm/lm.cpp` 的尾巴摆出来:

```cpp
unsigned long const kEntryTarget =
    cinux::boot::kHighHalfBase + cinux::boot::LoadWord(cinux::boot::kHandoffMailboxEntry);
unsigned long const kInfoAddress =
    cinux::boot::kHighHalfBase + cinux::boot::LoadWord(cinux::boot::kHandoffMailboxInfo);
asm volatile(
    "movq %[entry], %%rax\n"
    "movq %[info], %%rdi\n"
    "jmpq *%%rax\n"
    :
    : [entry] "r"(kEntryTarget), [info] "r"(kInfoAddress)
    : "rax", "rdi", "memory");
```

咱们看它从两只邮箱把门牌和单子的地址取出来,各加一个高半基——邮箱里躺的是物理地址,内核的世界观在高半,两头一拼才是跳转的目标。门牌装进了 rax,单子装进了 rdi,然后一条 jmp——执行流易了主。boot 的最后一行输出,永远停在 [lm] 那一行:它连再见都没说就把话筒递了。

为什么是一条普通的 jmp,不是远跳?远跳要带段选择子,而它的偏移量是 32 位旧世界的语法,装不下一个 64 位的高半地址——咱们从实模式一路用过来的远跳,偏移还都是低区的数。长模式里其实没有这个烦恼:CS 现在是 0x18 那个 64 位代码段,而长模式对代码段的基址整个忽略,同一个选择子之下的 rip,想指到多高的地址都指得到。所以咱们把 rip 的全 64 位装进 rax,走的是同段直跳,图的就是一步到位。rdi 也不是随手挑的寄存器:C++ 的调用约定,函数的第一个参数走 rdi——内核把自己的入口写成正经的 C++ 函数,单子当作第一个参数接住了。两边靠的是同一套约定,手就这么握上了。

还有最后一层:门牌为什么非要走邮箱?入口是运行期从鞋盒上读来的数,而 lm 的链和 stage2 的链互相看不见符号,编译期的时候谁也不认识谁,共用的语言只有 layout 里的数。于是内核改了版、门牌挪了窝,甚至整个把家搬到 8MB、16MB 的高处——这几处咱们都真搬过,门照开、船照到——boot 这边的链一个字节不用重编。鞋盒换了数,邮箱里就换了数,跳的还是那一跳。

一条 jmp——面板的主人换了。跳过去的那一头,接住执行流的第一行代码长什么样?咱们最后一节,去看内核怎么出生。
