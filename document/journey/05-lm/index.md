---
title: 05 · 长模式:在 32 位的世界里搭一座 64 位的桥
description: "保护模式只是中转站,咱们真正要去的 64 位还隔着一条分页的河。本站在 0x1000 起的三张表里铺一座恒等映射的桥,拨动 EFER.LME 的门闩,推门、远跳,让面板上多出第三行字,三行字分属三个世界。"
chapter: 5
order: 0
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - long-mode
  - paging
  - stage2
---

# 05 · 长模式:在 32 位的世界里搭一座 64 位的桥

<StationPanel tag="r05_long_mode" build="cmake -B build && cmake --build build --target run" />

上一卷收工的时候,咱们撂下过一句话:0x1000 起留着的三张页表地界,从画低内存地图的那天起就一直空着,也该住进去了。本站咱们就把它住满,然后再去推 64 位的门。本站咱们拿页表当桥,所以桥面得在推门之前铺好。门闩是 EFER 里的一个位,咱们不拨开它,门就推不动了。CR0 上的 PG 就是推门的那一下,推开的瞬间 CPU 立刻按新的翻译取指令,要是门后没有铺好的路,机器连一句遗言都不会给你留下。末了的那句远跳,把执行流真正送到桥的对岸。少了哪一样,或者次序走错了,机器的回应都是同一种:三重故障。构建那头咱们还有一件大事:64 位的代码要住进工程里的第四户编译世界,上一卷咱们数过三户,这一卷添的就是新的一户。它的链接也得自己住一条,因为链接器根本不允许 64 位的目标件进 32 位的链,咱们得把它编好、剥成裸字节,再当成数据拼进 16 位的镜像里。

<ChapterNav variant="sub">
  <ChapterLink num="1" href="01-bridge-and-gate" desc="32 位的世界已经安顿好了,可它只是中转站;长模式为什么必须架在分页上,本站的四样家当怎么接成一条过桥的路">桥、门闩,和推门的那一下</ChapterLink>
  <ChapterLink num="2" href="02-three-tables" desc="0x1000 起的 12KB 地界、三级表加 2MB 大页的最小组、恒等映射 0-8MB;拿一个地址真走一遍表,看清 PG 亮起之后 CPU 眼里的路">三张表,和门后必须有路</ChapterLink>
  <ChapterLink num="3" href="03-bit-vocabulary" desc="BitMask 立法:定义点只出现位置、从不出现数值;deposit 收的是帧号不是地址,脏低位在移位里就被丢弃;还有 unsigned long 在 32 位世界只有 4 字节、页表项却要 8 字节的那场暗亏">位置是代码,数值是断言</ChapterLink>
  <ChapterLink num="4" href="04-fill-the-tables" desc="当年 rep stosl 的清零换成 512 次 for 循环,三张表的地址从 layout 的法条派生;填完表,32 位世界把接力棒交给两个 extern &quot;C&quot; 的函数">把三张表填起来</ChapterLink>
  <ChapterLink num="5" href="05-efer-latch" desc="MSR 抽屉柜与 rdmsr/wrmsr 的约定;当年第一遍在开 PG 的瞬间三重故障,页表对、PAE 对,三寄存器联读把病根定在 EFER 的邻居位上">门闩住在 MSR 里</ChapterLink>
  <ChapterLink num="6" href="06-gdt-and-sequence" desc="三项的表加到五项,L=1 与 D=0 的硬关系,选择子算到 0x18;为什么这一次不需要第二次 lgdt;CR3、PAE、LME、PG、远跳,一步都不能错的序列真编真跑">五项的表,一步不能错的序列</ChapterLink>
  <ChapterLink num="7" href="07-own-chain" desc="64 位的目标件进不了 32 位的链,只能自己成链再剥成 68 个字节,0x9300 这个地址是实测里试出来的,先被 rodata 的尾巴拦了一回,又被看不见的 bss 拦了一回,去掉 KEEP 的对照实验绿着错">六十八个字节,自己一条链</ChapterLink>
  <ChapterLink num="8" href="08-landing-and-verify" desc="远跳落地,五个段寄存器换 64 位的衣服,栈顶回到原点;面板上多出第三行字,test_page 的五个哨兵,还有验收日可选的 GDB 取证:EFER=0x500、CS=0x18、CR0=0x80000011">落地,三行字,和验收</ChapterLink>
</ChapterNav>

本站咱们要干的活就是把桥铺到对岸。桥那头等着过桥的,就是咱们那个真正的 64 位内核:boot 把它从磁盘读进内存,它拿走问内存、配屏幕时存下的家底、再往高处安家,而终点的交接,就是把执行流亲手递到它的手里。
