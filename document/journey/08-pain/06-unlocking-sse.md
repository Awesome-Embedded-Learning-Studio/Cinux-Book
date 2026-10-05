---
title: 06 · 开禁
description: "动机就是自己要吃这口优化:内核旗组自立,-O2 和 -ftree-vectorize 进来,-mgeneral-regs-only 撤出。CR0 清 EM 置 MP,CR4 置 OSFXSR 和 OSXMMEXCPT,落在 KernelEntry 最顶上、任何 C++ 调用之前。isr.cpp 那一岛,独守通用寄存器。"
chapter: 8
order: 6
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - sse
---

# 开禁

咱们得把动机摆正,这一步是容易摆歪的。给内核开 SIMD 这样的事,听着像防御性的活:万一编译器生成了向量指令呢,咱们提前垫一层。咱们的立场比这直接,咱们是自己要吃这口优化的。两个 64 位数的加法,通用寄存器得干上两趟的活,xmm 一条指令就打包做完了。C++ 编译器是有自动向量化这本事的,可它的前提有两个:旗子上得开这个口子,地基那头也得赶在头里打牢。本卷把两样都办了。

咱们头一个看旗子。内核编译的这些日子,一直带着的是 -mgeneral-regs-only,不许碰向量寄存器的。这杆旗是从 boot 家族继承的:boot 的 16 位、32 位、64 位三个世界本就不该有向量,实模式的世界里谈什么 SIMD。内核跟 boot 共用一套旗组过日子的时候,这杆旗就一起罩了下来。本卷内核自立了门户,单立的旗组长这样:

```cmake
add_library(cinux_kernel_flags INTERFACE)
target_compile_options(cinux_kernel_flags INTERFACE
    -m64
    -O2
    -ffreestanding
    -fno-pie
    -fno-pic
    -fno-stack-protector
    -fno-asynchronous-unwind-tables
    -fno-ident
    -fno-exceptions
    -fno-rtti
    -fno-threadsafe-statics
    -mno-red-zone
    -ftree-vectorize
)
```

INTERFACE 形式的库,自己一个字节的代码都是不产的,只把编译的口径递给挂它的人。名字单看是新的,里子您大部分认得:freestanding、不开异常、不开 RTTI,这些是内核世界的老家底,过去是从 boot 家族顺来的,如今写进了自家的门牌。真正的变动就三笔:-O2 换掉了 -Os,-ftree-vectorize 进了驻,而 -mgeneral-regs-only 撤了出去。boot 的三模照旧 -Os、照旧禁浮点,两家人从此各挂各的旗。

这里有个实测出来的冷知识,值得咱们记下:-Os 下,连显式写出来的 -ftree-vectorize 都不开工,它的成本模型便宜到把向量的数全盘否掉,一条都是不肯生成的。而真要吃向量,咱们就得上 -O2 这一档。换了之后咱们数过,内核二进制里 xmm 指令实打实落了 17 处,编译器是真的在干活。-O3 咱们也试过,xmm 还是停在 17 处的规模,体积也分毫不差的,那咱们照大伙的惯例留在了 -O2。

咱们再看地基。CPU 认不认这类的指令,跟编译器是不相干的,管这事的是两个控制寄存器里的几粒位。CR0 的 EM 位说的是“没有协处理器”,而它一亮起来,浮点指令进门喊的就是 #NM。MP 是跟它配套的老开关,管 WAIT 那类指令听不听 TS 位的话。CR4 那头两粒位更要紧:OSFXSR 不置的话,任何一条 SSE 指令都会被当成无效的指令,直接判下的就是一个 #UD,真正拦在咱们面前的就是这一粒。OSXMMEXCPT 不置的话,#XM 的信送不进 19 号,SIMD 的数学事故会被改判成无效指令。所以开禁一共四笔:CR0 清 EM、置 MP,CR4 那头要置的是 OSFXSR 和 OSXMMEXCPT 两粒。这几笔咱们是显式地写的,不靠开机默认值里它们碰巧是什么样,自家的地基,由咱们自己来声明。

为什么这几笔必须落在文件最顶上、落在任何一句 C++ 调用之前?您想:开禁之后,任何一个编译件在理论上都是躲不开 SSE 的,初始化自家的那一段代码也不例外。要是跑了初始化、再写控制寄存器,初始化自己就可能成了头一个受害者,而且死得没有名字:那时候连报错的桩都还没上岗。考古箱里的当年第一遍,在 -O2 下被动地挨过这一下,优化进来了,地基没人打,引导期说崩就崩的性子,后来才补的初始化。这一遍咱们反了过来:地基打在前,优化放在了后头,而这样的顺序本身,就是咱们的防线。

有一个例外咱们必须交代,不然上面的话就说过头了:isr.cpp 这个编译件,源码级地独保着 -mgeneral-regs-only。这是 interrupt 属性的硬要求,中断的调用约定不给向量寄存器做保存恢复,桩里要是出了 SSE,现场就毁了。GCC 在这儿把关:备料的时候咱们拿一个独立的小工程试过,旗子上不关向量的话,interrupt 函数当场就拒编了,报错是实打实的。所以这一卷之后的内核是这么一副身材:整体的代码吃着向量优化,唯独桩这一座小岛上的日子,还是守着通用寄存器过的。岛是不大的,边界却是清清楚楚的。

上一节打过照面的 #XM,如今有名有实了:SSE 在门内,19 号那行的 SIMD Floating-Point Exception,从死向量变成了在岗的哨。地基打完,旗子也换完了,剩下的活,是把这些新家什排进每天的日程,到了下一节,咱们就起居重排。
