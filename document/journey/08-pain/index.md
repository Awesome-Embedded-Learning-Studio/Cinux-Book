---
title: 08 · 会疼:自己的表,有名有姓的疼
description: "上一卷清完了嗓子和家底,这一卷轮到表:段表是踩着 boot 的 0x18 过日子,异常表三十二个号只挂了五桩,疼了也只有一行遗言。本卷内核自立门户:GDT 三槽、IDT 二百五十六格、编译器生成的桩、喊得出名字的异常现场,外加把 SIMD 请进门。"
chapter: 8
order: 0
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - gdt
  - idt
  - exceptions
---

# 08 · 会疼:自己的表,有名有姓的疼

上一卷收工的时候,咱们许过愿:下一卷的正主,是教内核搭一张被打扰的表。真到了动工,咱们把现状盘了一遍,盘出来的却是两笔亏欠。头一笔欠的是表:内核每天踩着的 CS 是 `0x18`,记着它的却是 boot 的表——保护模式的那一卷立了三项,过桥进长模式的那一卷添到五项,产权从头到尾都攥在 boot 的手里。内核住的房子,地契押在了别人家。第二笔欠的是疼:异常的号一共三十二个,挂了桩的只有五个,还是内核出生那一卷手搓的汇编前哨。别的错真冒出来了,面板上交得出的只有向量号和 rip 两笔,想多说一个字都不行了,而再往下,就是三重故障的黑屏了。

本卷咱们就办两头的活:一头把门户自立起来,一头让疼喊得出自己的名字。咱们头一件事是给内核立自己的 GDT,三格就齐了——null 守一格、内核代码占一格、内核数据收一格,多一格都不要了。然后把中断的表铺满二百五十六格,合起来的 4KiB 整张住 .bss,盘上是一个字节都不花的。桩的活,咱们让编译器写:GCC 的 interrupt 属性配上模板,当年五百行宏阵的活,如今一行宏都不写了。报告也跟着变厚了:六件现场、外加一整张三十二个名字的名表。还有一件要单独说的活,咱们把 SIMD 请进内核——旗组自立了,SSE 也开了禁,地基打在了前头,优化是随后才放进来的。收工的时候,面板的第二行会多出一句自我报告:表是自己的了,SSE 也进门了。验收那天的重头戏是一针 ud2,咱们故意往代码里塞一条无效指令,看内核把疼一五一十地喊出来。

<StationPanel tag="r08_idt" build="cmake -B build && cmake --build build --target run" />

<ChapterNav variant="sub">
  <ChapterLink num="1" href="01-a-borrowed-table" desc="selector 0x18 一直指着 boot 立的表,内核住的房子的地契在别人手里。异常只有五桩手搓前哨、一行遗言,再往下是三重故障的黑屏。考古箱里,这套东西当年写了两遍">表是借来的</ChapterLink>
  <ChapterLink num="2" href="02-three-slots" desc="GDT 的最小形态:null、内核代码、内核数据,三槽齐了。两套 scoped enum 的位词汇不许混或,constexpr 工厂逐位对数的标准答案,lgdt 加远跳加数据段重载,选择子从 0x18 换到 0x08。当年一次铺七槽的密度病,这次不犯">三槽的算盘</ChapterLink>
  <ChapterLink num="3" href="03-two-hundred-fifty-six-gates" desc="IDT 从三十二格铺到二百五十六格,16 字节一门、4KiB 住 .bss。EncodeGate 纯函数切地址,host 五根哨兵上硬件之前拴死字段。空门不瞎响,被踩到了会替缺席者报名">二百五十六桩</ChapterLink>
  <ChapterLink num="4" href="04-stubs-without-macros" desc="当年 interrupts.S 两个宏加十五桩手搓的五百多行,这一遍一行宏都不写:interrupt 属性生成整套桩,模板实例化把向量号烤进去,伪补零交给编译器,三十二桩折叠安装">让编译器写桩</ChapterLink>
  <ChapterLink num="5" href="05-pain-with-a-name" desc="三十二个向量名表,越界老实回 Unknown。六件现场:名字、rip、cs、rflags、rsp、err,每件为什么值这一行。#NM 和 #XM 提前打个照面,它们跟 SIMD 指令的命运拴在一处">疼要喊得出名字</ChapterLink>
  <ChapterLink num="6" href="06-unlocking-sse" desc="动机就是自己要吃这口优化:旗组自立,-O2 和向量化进来,-mgeneral-regs-only 撤出。CR0 清 EM 置 MP,CR4 置 OSFXSR 和 OSXMMEXCPT,落在 KernelEntry 最顶上、任何 C++ 调用之前。桩那一岛独守通用寄存器">开禁</ChapterLink>
  <ChapterLink num="7" href="07-the-new-routine" desc="entry.cpp 从杂货铺瘦成日程表:清 .bss、GDT、装桩、lidt、Main、Halt,一步一讲究——门里的 0x08 在 boot 的旧表里是 32 位代码段,通电次序错一步就是黑屏">起居重排</ChapterLink>
  <ChapterLink num="8" href="08-audit-and-one-ud2" desc="构建、测试、tidy、QEMU 四路全过,面板五行逐字对数,32348 页的来龙去脉。一针临时注入的 ud2,六件现场全对,cs=8 顺带证明表真换了。考古:当年四月二十的单日九站,和这一卷对那天的回答">验收,和一针 ud2</ChapterLink>
</ChapterNav>

表立好了,名字也挂上了,可中断的门是关着的——从保护模式那一卷的 cli 到今天,这门是一步都没松过的。上一卷把搭表和学会听一口气许给了下一卷,真到了排次序,咱们把它拆成了两课:表的功课在前,听的功课在后,这一卷只办前面的那一件。听是下一卷的事:外线的芯片、时钟的第一声嗒、头一回 sti,都是那一卷的活,键盘的事还得再等一站。眼下咱们要办的事只有一件——把疼教明白。
