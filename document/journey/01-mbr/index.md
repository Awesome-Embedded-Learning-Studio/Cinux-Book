---
title: 01 · MBR 专题:从引导扇区到第一段 C++ 代码
description: "512 字节的引导扇区,通篇用 C++ 写:-m16 让引导代码告别 .S 文件,内联汇编跟 BIOS 打交道,链接脚本把家安在 0x7C00,一条命令在 QEMU 里拉起最小的回声。"
chapter: 1
order: 1
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - mbr
  - real-mode
  - bios
---

# 01 · MBR 专题:从引导扇区到第一段 C++ 代码

<StationPanel tag="r01_mbr" build="cmake -B build && cmake --build build --target run" />

上一站咱们把武器磨好了,这一回轮到咱们点火。机器通了电,BIOS 把磁盘的第一扇区读进内存,顺势跳了进去——从那一跳开始,跑的就是咱们的代码。本站的引导代码通篇是 C++,连 512 字节的 MBR 都是一个 `.cpp`。其实汇编没有消失,只是退到了一条一条的指令上。收工的时候,QEMU 里的机器会回咱们三行字。

<ChapterNav variant="sub">
  <ChapterLink num="1" href="01-why-now" desc="武器造好了该点火;boot 里没有调试器搭手,咱们顺手试掉『512 字节装不下 C++』这句老话">为什么是现在</ChapterLink>
  <ChapterLink num="2" href="02-real-mode" desc="0x7C00 的约定、段乘十六加偏移的一兆字节、低内存的地图,和只在实模式应答的 BIOS">一兆字节里的世界</ChapterLink>
  <ChapterLink num="3" href="03-cpp-in-16-bits" desc="-m16、入口桩、extern &quot;C&quot; 与 0xE9 端口:引导扇区里活下来的 C++ 长什么样">用 C++ 写引导扇区</ChapterLink>
  <ChapterLink num="4" href="04-read-and-jump" desc="DAP 十六个字节逐字段过一遍,clobber 列表看住寄存器,ljmp 把 CS 归一,再把世界交给 stage2">跟 BIOS 打交道</ChapterLink>
  <ChapterLink num="5" href="05-build-wall" desc="512 字节红线写成构建期断言;把 bin 顶到 536 字节的 PIE,还有 rm 掉产物之后要能自愈的依赖图">构建侧的墙</ChapterLink>
  <ChapterLink num="6" href="06-echo" desc="stage2 十六行,一条命令拉起 QEMU,debugcon 落下三行字,机器说出启动以来的第一句话">机器开口应答了</ChapterLink>
</ChapterNav>

下一站咱们要让 stage2 在铺好的世界里安家落户,它的活,也比打一行标记多起来了。
