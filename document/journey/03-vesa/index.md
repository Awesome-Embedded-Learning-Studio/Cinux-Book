---
title: 03 · VESA 配屏:向显卡要一块画布
description: "起居的活儿干到只剩屏幕这一件,它却还借 BIOS 的文本模式撑着。本站请出 VBE,把 1024x768 的线性画布切过来,framebuffer 的参数趁 BIOS 还应答一并存好。"
chapter: 3
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
  - vesa
---

# 03 · VESA 配屏:向显卡要一块画布

<StationPanel tag="r03_vesa" build="cmake -B build && cmake --build build --target run" />

上一站收工的时候,stage2 的起居室里,栈立好了,A20 的门开了,七条内存图谱也躺进了存档。剩下来的只有屏幕这一件,它却还是 BIOS 借给咱们的:那块 80 列的文本模式由 BIOS 维持,咱们写的每一行字都是借它的光。本站咱们把这块屏办完,向显卡要一块图形模式的画布,也就是线性帧缓冲的地基。本站要过的三问,问的是 4F00 拿模式列表、4F01 逐个查模式参数,末了一问才是 4F02 带着线性帧缓冲位切过去。屏幕之外还有几件事要一起办:两块 BIOS 收货区的字段布局、把远指针拍平了再去读模式列表的循环,还有编译期能查的那批判定,连带着跟着回改的低内存地图。本站还欠着一笔旧债:当年笔者给 0x118 随手贴上的 32 位色标签,这次咱们要按模式列表一个个问回来。

<ChapterNav variant="sub">
  <ChapterLink num="1" href="01-why-now" desc="屏幕还是 BIOS 借给咱们的,文本模式说没就没;配屏为什么只能排在现在,画布参数为什么只存不用">屏幕还是借来的</ChapterLink>
  <ChapterLink num="2" href="02-why-enumerate" desc="当年一个想当然的 0x118 换来双重打脸,维基上的常用号在 QEMU 里根本不存在;模式号写不得,枚举才是正路">模式号,想当然不得</ChapterLink>
  <ChapterLink num="3" href="03-second-bios-door" desc="4F00 递上签名、4F01 逐个模式问详情、4F02 带着 bit14 切过去;AX 全等 0x004F 的回执,和 E820 的 carry/SMAP 正好凑成对照">第二次进 BIOS 门</ChapterLink>
  <ChapterLink num="4" href="04-wire-format" desc="512 字节与 256 字节两块缓冲的镜像结构、尾巴必须补齐的原因,还有 FrameBufferInfo 十七个字节里藏的对齐漂移">两块缓冲,十七个字节</ChapterLink>
  <ChapterLink num="5" href="05-enumerate" desc="远指针拍平成平坦地址,可段值这一家恰好是零;列表住哪儿、为什么那一乘法不能省,还有 128 项护栏与 32 位想要 24 位保底的双档挑法">模式列表,逐个问详情</ChapterLink>
  <ChapterLink num="6" href="06-test-vesa" desc="判定逻辑升上编译期,测试文件只留两个哨兵;一个 constexpr 工厂函数,把指定初始化器和告警集的两头都伺候好">boot 血统的第二件测试</ChapterLink>
  <ChapterLink num="7" href="07-budget" desc="动工前那版试写探测把 8 扇区的预算顶穿了,构建闸当场拦下;三个固定低地址和图上的占位一起退役,预算抬到 12 扇区">预算顶着上限,地图跟着回改</ChapterLink>
  <ChapterLink num="8" href="08-black-screen" desc="模式一切,屏幕黑了,debugcon 里字还在;逐行认面板输出,把终止符、bit14、VBE2 签名三件事按现场交代掉">屏幕黑下去,字还在</ChapterLink>
</ChapterNav>

下一站咱们要跟 16 位实模式那套段:偏移的世界道别了。保护模式要用的全局描述符表(GDT),到时候由咱们自己搭。所以配屏排在了它前头——进了保护模式,BIOS 就再也喊不应了,该问的参数,咱们都得趁现在问到手。
