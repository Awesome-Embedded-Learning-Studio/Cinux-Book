---
title: 04 · 保护模式:推开 32 位的大门
description: "起居三件事和一块屏都齐了,可它们全跑在 16 位实模式里。本站建一张三项的扁平 GDT,拨动 CR0 上的 PE 位,再用一句远跳跨进 32 位世界——跨过去,BIOS 就再也叫不应了。"
chapter: 4
order: 0
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - gdt
  - protected-mode
  - stage2
---

# 04 · 保护模式:推开 32 位的大门

<StationPanel tag="r04_pm" build="cmake -B build && cmake --build build --target run" />

上一站收工的时候,stage2 的起居室已经齐整了:栈是咱们自己的了,A20 的门开着,七条内存图谱也躺进了存档,连屏幕的画布都挂上了墙,framebuffer 的参数一样样问回来、存好了。可是咱们心里都清楚,这些家业全部跑在一个 16 位的旧世界里:算地址靠的还是段寄存器左移四位再拼偏移,摸得着的上限是 1MB,而 BIOS 的中断还随时候着命。本站咱们要把这个世界换掉。要办的四件事,一件都省不得:用 `cli` 把中断的门关上,建一张 GDT 立好新世界的底数,拨动 `CR0` 上的那个开关,末了用一句远跳跨过门槛。咱们跨过去之后,BIOS 就再也叫不应了——所以上一站咱们才赶着把该问的参数全问完了。本站还有一件构建层面的大事:32 位的代码要住进自己的编译世界,跟 16 位的世界隔着一句远跳相望。

<ChapterNav variant="sub">
  <ChapterLink num="1" href="01-why-now" desc="家业都齐了,却全跑在 16 位实模式里;1MB 的天花板、不设防的段式寻址、随时候命的 BIOS 中断,以及为什么这一次告别是不可逆的">最后一次跟 BIOS 说再见</ChapterLink>
  <ChapterLink num="2" href="02-gdt-design" desc="三项的小表、选择子就是条目偏移、base 全零 limit 全开的扁平模型;段在咱们手里变得透明,地址自己就是线性地址">一张三项的扁平 GDT</ChapterLink>
  <ChapterLink num="3" href="03-gdt-bytes" desc="packed 结构体逐字段对上硬件的八个字节,0x9A 一位一位认,limit 两段拼出 4GB;constexpr 工厂让当年注释里的笔误无处藏身">八个字节,一位一位认</ChapterLink>
  <ChapterLink num="4" href="04-two-worlds" desc="当年的 .code16/.code32,如今变成文件级的两个编译世界;ljmp 是边界,一个 OBJECT 库把边界固定进构建系统,头文件里的内联函数每个世界白拿一份">第二个编译世界</ChapterLink>
  <ChapterLink num="5" href="05-switch-sequence" desc="cli、lgdt、CR0.PE、远跳,四步一步不能换序;DS=0 这个当年的教训,如今已经写成断言长在代码里;还有 16 位远跳偏移划下的 64KB 界,与实模式加载路径圈定的 1MB 地界">切换序列,一步都不能错</ChapterLink>
  <ChapterLink num="6" href="06-pm-entry" desc="远跳只刷了 CS,另外五个段寄存器还得手动换干净;新栈搬到 0x90000,整行的打印从 debugcon 出来,CPU 换了世界,输出不掉线">跨过门槛之后</ChapterLink>
  <ChapterLink num="7" href="07-verify" desc="test_gdt 的三个哨兵、面板上隔着一个世界的两行输出、GDB 里从 ip 到 eip 的寄存器改名,还有 lgdt 那场延迟爆炸的旧事">验证与调试现场</ChapterLink>
  <ChapterLink num="8" href="08-wrapup" desc="表、开关、边界、新世界,四样都立住了;32 位保护模式只是中转站,x86_64 的故事在下一站的分页和长模式里">中转站,不是终点</ChapterLink>
</ChapterNav>

咱们再往下一站,就要进 64 位的长模式了。那边的资格贵得多:分页得建起来,`CR4.PAE、EFER.LME、CR0.PG` 要一个个地拨过去,末了还得再来一次远跳。0x1000 起的三张页表地界眼下还空着——那是下一站的活。
