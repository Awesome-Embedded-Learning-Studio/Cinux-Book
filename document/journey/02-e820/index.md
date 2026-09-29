---
title: 02 · stage2 起居:自己的栈、打开的 A20、一张内存图
description: "MBR 铺平的世界是借来的:栈要搬进自己的家,A20 要请 BIOS 打开,机器有多少内存要问清楚存好。收工的时候,起居序列一口气落下来,七条内存图谱摊在终端里。"
chapter: 2
order: 0
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - stage2
  - real-mode
  - bios
---

# 02 · stage2 起居:自己的栈、打开的 A20、一张内存图

<StationPanel tag="r02_e820" build="cmake -B build && cmake --build build --target run" />

上一站收工的时候,stage2 只应了一声 alive——家当薄得可怜。本站咱们让它搬进自己的家:栈立在有自己的下界的地方,A20 咱们请 BIOS 打开,机器的内存图谱一条条问出来、存好。收工的时候还是同一道命令,终端里落下来的不再只有三行,而是一整串起居序列——最后那七行十六列的地址,就是机器交给咱们的第一份家底。

<ChapterNav variant="sub">
  <ChapterLink num="1" href="01-why-now" desc="MBR 铺平的世界是借来的:栈、地址线、内存家底三样都得变成 stage2 自己的;当年这一切挤在同一天干完,这一遍摊开来消化">回声长成居所</ChapterLink>
  <ChapterLink num="2" href="02-the-map" desc="当年随手定的栈当天夜里崩在一个 ret 上;这一遍把 boot 全生命周期的低内存一次画全,区间收拢成值类型,断言链看图">把地图一次画全</ChapterLink>
  <ChapterLink num="3" href="03-own-stack" desc="cli 打头、mov ss 之后 CPU 送的一拍、0x7000 的来历,和顶层 asm 里手抄的数">搬进自己的栈</ChapterLink>
  <ChapterLink num="4" href="04-a20" desc="8086 的回卷、键盘控制器上搭的门、三种开门法,和咱们为什么只选 BIOS 那一路">一根地址线的老故事</ChapterLink>
  <ChapterLink num="5" href="05-ask-the-bios" desc="INT 15h AX=E820 的调用约定、SMAP 三道检查、EBX 续传令牌,还有 clobber 在这里的第二层含义">问 BIOS 要一张内存图</ChapterLink>
  <ChapterLink num="6" href="06-the-archive" desc="24 字节 packed 条目、住进头文件的十六条断言、raw type 的保守翻译,以及 16 位世界里打 64 位数的两件麻烦">二十四字节,一条一条确定下来</ChapterLink>
  <ChapterLink num="7" href="07-first-boot-test" desc="编译期能查的全升成 static_assert,测试文件只剩哨兵;boot 的头文件头一回进测试流水线">boot 血统的第一件测试</ChapterLink>
  <ChapterLink num="8" href="08-alive" desc="一条命令跑完起居序列,七条图谱陪着机器认一遍,stage2 从五十来字节长到三千多字节">起居序列,一口气</ChapterLink>
</ChapterNav>

下一站咱们去配屏:VBE 三步走,把一块图形模式的画布要下来,framebuffer 的参数也趁 BIOS 还应答,咱们一并问清、存好。
