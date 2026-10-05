---
title: 03 · 二百五十六桩
description: "异常表从三十二格铺到二百五十六格:16 字节一门,4KiB 整张住 .bss,盘上一个字节不花。EncodeGate 把一个 64 位地址切成三截装进门,type_attr 全表只认 0x8E 的中断门。空门不瞎响,被踩到了会替缺席者报名。"
chapter: 8
order: 3
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - idt
---

# 二百五十六桩

段表是自己的了,轮也轮到了中断表。老表的寒酸咱们头一节盘过了:三十二格,挂了五桩。新表咱们一次铺满:二百五十六格,每格的身量是十六字节,合起来的正好是 4KiB,整张都住进了 .bss,盘上一个字节都不用为它花——和上一卷 PMM 那半兆名册是同一条纪律:内存里占地的院子是自家的,盘上的镜子不照它。

为什么是二百五十六?这个数不是咱们拍的:CPU 的向量号拢共就这么多。0 到 31 是架构异常的地界,32 往上是外线的地界,下一卷的时钟、再往后的键盘,将来都是从这片进的。咱们现在把整张表立起来,门牌全数挂在了前头,住户是后搬的,等下一卷真要接外线的时候,装门就行了,不用再动表的骨架了。

门里装什么,咱们看 GateEntry 的字段就清楚了:

```cpp
struct [[gnu::packed]] GateEntry {
    unsigned short offset_low;
    unsigned short selector;
    unsigned char  ist;
    unsigned char  type_attr;
    unsigned short offset_mid;
    unsigned int   offset_high;
    unsigned int   reserved;
};
```

处理程序的地址被切成了三截——低十六位、中十六位、高三十二位,散在门的不同角落,中间夹着的是选择子、ist 和 type_attr。一个好好的 64 位地址,为什么要切成三截住?没有为什么,CPU 定的格式,咱们照着装。EncodeGate 是个纯纯的 constexpr 函数:进的是一个地址,出的是十六个字节,一个副作用都是没有的。地址怎么切的、字段怎么落的,全都在函数体里明摆着了:

```cpp
constexpr GateEntry EncodeGate(unsigned long long handler, unsigned short selector,
                               unsigned char ist, unsigned char type_attr) {
    return GateEntry{.offset_low  = static_cast<unsigned short>(handler & 0xFFFF),
                     .selector    = selector,
                     .ist         = ist,
                     .type_attr   = type_attr,
                     .offset_mid  = static_cast<unsigned short>((handler >> 16) & 0xFFFF),
                     .offset_high = static_cast<unsigned int>(handler >> 32),
                     .reserved    = 0};
}
```

咱们单看三截的切法,就是三次移位加掩码的活,别的没了。这样的函数为什么值得单独立一个名字?因为它是纯的:同样的地址进去,同样的十六字节出来,全局是不碰的,状态也是不看的。这样的纯,才搬得进 host 的测试,这是下一层红利的前提。

type_attr 咱们全表只认一种值:0x8E,是在场、DPL0、中断门的组合。中断门有个要紧的脾气,咱们选它主要就是冲这个:进门的那一拍,CPU 自动把 rflags 里的 IF 清了,处理程序天然在关中断的环境里跑,出去 iret 的时候再自动恢复。这跟咱们至今的纪律严丝合缝:cli 从保护模式的那一卷立到现在,门本来就是没开过的,门型自然挑最稳的。陷阱门这样进了不清 IF 的脾气,等哪天真有多级中断要嵌套了,咱们再考虑给它派用处。

编码进了纯函数,红利是在 host 那头兑现的。test_idt 的五根哨兵,上硬件之前就把字段拴死了:GateEntry 在 64 位平台上必须正好 16 字节,pack 过了一个字段都不能歪。一个手工地址 0x1122334455667788 编进门,切出来的必须是 7788、5566、11223344 三截,一格的差都不能有。selector、ist、0x8E 落的也是各自的位置。type_attr 的位模式一位一位对:在场一位、特权两位、门型四位。内核住的 0xFFFFFFFF80200000 这样的高地址,编进了门之后,一个位都是不能丢的,这根哨兵专防的病,就是低地址测得出、高地址真翻车的暗患。这套路数是从页表条目那一卷传下来的老路数:编码进纯函数,host 的哨兵在前、硬件在后,表条目咱们从来不裸着上机器。

空门的脾气咱们也交代清楚。二百五十六格的表里,咱们眼下装了桩的是 0 到 31,32 往上的格子是空的。空门是不会瞎响的,可真被踩到了,比如哪天哪个向量被一条 int 指令叫到了,门是不在场的,CPU 就报出了 #GP,向量号藏在了错误码里,右移三格就见了真身,咱们的转储会替缺席者把名报出来。所以空表不撒谎:它只是暂时没住户,门牌簿是全的。

装表的活儿简单:InstallGate 一格一格地写,LoadIdt 一句 lidt——就把表的地址装进了 CPU。您要是觉得眼熟,是不用奇怪的,上一节 lgdt 递给 CPU 的也是同款的两笔:表的身长,表的地址。lgdt 和 lidt 打的是兄弟般的一对:一个领的是段世界,一个领的是打断的世界,交接的格式都长一个样。从 lidt 的这一拍起,CPU 再被打断的时候,认的就是自家的表了。表有了,门认了。剩下的活是往门里装桩,而这活咱们一行宏都不写,全交给编译器的手艺。
