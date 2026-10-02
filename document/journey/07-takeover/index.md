---
title: 07 · 内核接管:自己的嗓子,自己的家底
description: "内核活着,可它的每一句话都是从 QEMU 的后门递出去的,真正的开发板上没有这扇门。本卷上两道菜:把话说在 COM1 的 16550 上,boot 与内核全链单流,debugcon 退役。把内存记进一页一 bit 的位图名册,单子上的 E820 图谱头一回被内核侧真正消费。"
chapter: 7
order: 0
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - serial
  - pmm
---

# 07 · 内核接管:自己的嗓子,自己的家底

上一卷收工的时候,内核当了家:会打三行字,会扫自己的院子,出事的早上喊一声 rip。可咱们冷静下来一数,发现它还欠着两件没办的事。头一件欠的是嗓子:它说的每一句话,都是从 0xE9 上的 debugcon 递出去的——QEMU 专属的外设,真正的开发板上根本没有——拔掉虚拟机的拐棍,内核当场就成了哑巴。另一件欠的是家底:单子上誊来的内存图谱,boot 只帮咱们问过一回,图谱在内核的手里就一直闲着,您要是问机器还有多少页能分、哪些页有了主,内核自己是一问三不知的。所以本卷就办这两件事:把话说在真正的机器也认的串口上,把内存记在自己的名册上,一件一件地办。

这一路咱们顺手要办的体面活儿还有几样。内核树添了 driver 和 mm 两个器官目录,串口住进了四层楼,私有也就一律进了 .cpp。console 的老红利吃到头,一份脸面分了家,成了两户人家。MBR 退回了纯装载器,面板头部的两行字也体面退了场。九判里那道报错了地方的顶,也收到了门真正盖到的地方。收工的时候,QEMU 的命令换了班,-serial stdio 接了棒,咱们整条链从 stage2 到内核,共用上了同一张嘴——面板的末尾多出那行 pmm,机器的家底,头一回由住在里面的住户自己报了出来。

<StationPanel tag="r07_takeover" build="cmake -B build && cmake --build build --target run" />

<ChapterNav variant="sub">
  <ChapterLink num="1" href="01-borrowed-voice" desc="内核的每一句话都是从 0xE9 后门递出去的,debugcon 是 QEMU 专属的外设,真正的开发板上没有这扇门。本卷两道菜宣布:话说在真串口上,内存记在自己的名册上">嗓子是借来的</ChapterLink>
  <ChapterLink num="2" href="02-com1-and-seven-writes" desc="COM1 从 0x3F8 起家,七张初始化条子、DLAB 换脸、除数与 8N1、THRE 的一问一递。后门只收不答,驱动头一回让设备回了话">真串口,七写开嗓</ChapterLink>
  <ChapterLink num="3" href="03-a-vocabulary-for-ports" desc="kernel/driver 开张,四层楼各干各的:print 排版、console 薄脸面、serial 藏起寄存器与表、io 握端口。PortWrite 让初始化序列成了能读的数据手册,裸 outb 关进 io.cpp">把 outb 藏进词汇表</ChapterLink>
  <ChapterLink num="4" href="04-two-houses-one-face" desc="header-inline 的红利吃到 .cpp 就到头,跨模式链符号的静默死转实录,boot/early 自足拷贝登场。MBR 回纯装载器,面板头部两行字退场的取舍">一份脸面,两户人家</ChapterLink>
  <ChapterLink num="5" href="05-voice-on-the-wire" desc="-serial stdio 接棒,debugcon 退役,全链单流。第一次开机,[lm] 之后是一场静默的官司——鞋盒短报 12 个字节,三张条子没过河,字符全进了除数锁存器">换嗓子,开机</ChapterLink>
  <ChapterLink num="6" href="06-one-bit-per-page" desc="kernel/mm 开张,单子上的 E820 图谱头一回被内核侧真正消费:起手全记有主,usable 逐条放行,低 1MiB 一刀,内核登记在册。半兆名册住 .bss,盘上一个字节不为它花">一页一 bit 的名册</ChapterLink>
  <ChapterLink num="7" href="07-order-from-day-one" desc="allocate_pages 的 order 从第一天就带连续语义,封顶 9 正好一张 2MiB 大页。pmm_config、pmm.hpp、pmm.cpp 三份文件各守一个家。位图只是第一个后端,当年三改的编年史,外加门顶的诚实收尾">接口一次作对</ChapterLink>
  <ChapterLink num="8" href="08-audit-and-cadence" desc="host 十三件测试全过,32350 页的数字一项项核对,probe 落在 1MiB。考古箱里翻出当年停摆的 23 天">验收,和卡住的日子</ChapterLink>
</ChapterNav>

内核的话上了真串口,内存的家底也记在了自己名下。上一卷结尾点过的那几件活,这一卷清了一件,下一件轮到中断的表——五桩异常守的只是前哨,全量表、时钟、键盘都排在它的后面。盘驱动和换 ELF 的那一刀,这一卷咱们没碰,该出场的时候它们自己会来。机器安顿在了 Halt 里,串口线上的字,送到了最后一行。
