---
title: 06 · 第一个内核:把执行流递到它手里
description: "桥铺好了,对岸的主人还躺在磁盘上。本站在跟 BIOS 告别之前把内核装进内存:一扇 64KB 的暂存窗、一趟 32 位的摆渡、一只写着鞋码的鞋盒;再把手绘的内存图谱和画布参数誊进交接单,给三张表开两扇门,末了一跳,面板上多出第四行字。"
chapter: 6
order: 0
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - loader
  - stage2
---

# 06 · 第一个内核:把执行流递到它手里

<StationPanel tag="r06_first_kernel" build="cmake -B build && cmake --build build --target run" />

上一卷收工的时候,咱们撂下过一句话:桥那头等着过桥的,就是咱们那个真正的 64 位内核。它拿走问内存、配屏幕时存下的家底,再往高处安自己的家,而终点的交接,就是把执行流亲手递到它的手里。咱们要办的四件事,就是本站的全部:boot 要赶在跟 BIOS 的告别之前,把磁盘上的内核装进内存。要把它问回来的家底誊成一份交接单。要给上一卷铺的三张表开两扇门,好让内核住进高处的地址。末了一跳,从此面板上的字换了主人。装载的次序在本站升成了原则:告别是一次性的,门在身后关上了,所以凡是还要用到 BIOS 的活,都得赶在告别之前干完——本站咱们会看到,次序怎么把整条装载流水线的形状定了下来。

构建那头的大事,咱们得单说一件:`kernel/` 这棵树在本站出生。工程里从上一卷起住下了四户编译世界,本站添的是第五户:按 `-m64` 编、按内核的代码模型链、住高半地址的一户。它跟当年的那 68 个字节一样,得用自家的一条链、剥成裸字节,不过这一回,它不再是拼进 stage2 镜像里的一包数据,而是躺在磁盘上、等 boot 亲手来接的正主。

<ChapterNav variant="sub">
  <ChapterLink num="1" href="01-the-fourth-line" desc="面板的尾巴停在三行,第四行还不存在,它的主人躺在磁盘第 32 号扇区里;当年装载是三段接力,小内核长成半个操作系统才搬得动大内核,同一件事写了好几遍的病根在结构不在人">第四行字,是谁打的</ChapterLink>
  <ChapterLink num="2" href="02-window-and-ferry" desc="实模式 64KB 的牢笼、0x10000 起的暂存窗、保护模式闪进闪出的一窗一往返;两个编译单元各守母语,GDT 加到六项,归程那条带前缀的 ret;还有 Linux、GRUB 和老自举年代的三种玩法对照">一扇窗,一个来回</ChapterLink>
  <ChapterLink num="3" href="03-size-on-the-box" desc="头 40 个字节的镜像自述头七个字段,数字全由链接脚本算出来;高半的 VMA 是世界观,AT 里的 0x200000 是物理的家;file_size 与 mem_size 是段尺寸算术,BSS 从此失去特殊地位">鞋码写在鞋盒上</ChapterLink>
  <ChapterLink num="4" href="04-proof-before-loading" desc="没有预设的最大内核尺寸,只有可装载性证明:九道检查从魔数一路到脚印重叠,区间包含而不是总量求和;当年内核压着自家栈崩在一个 ret 上的事故,如今是进场前的最后一判">上限是验出来的</ChapterLink>
  <ChapterLink num="5" href="05-read-validate-ferry" desc="读一扇区、验九判、127 扇区一窗循环摆渡、头一窗的魔数回执;溢出安全的四则运算,和 stage2 收尾的次序——问完内存、配完屏、装完内核,才说再见">四段流水线</ChapterLink>
  <ChapterLink num="6" href="06-the-handoff-record" desc="ABI 前门头三样、整份誊抄的内存图谱、带六个位域的画布参数、内核自己的四个数;交接单住在 kernel/boot 的一份头文件里,两只低址信箱负责跨链递话">家底的交接</ChapterLink>
  <ChapterLink num="7" href="07-two-doors-last-jump" desc="三张表从临时桥长成交接门:PML4 的第 0 项与第 511 项指着同一张表,高半别名恒等于物理地址;门数按内核的尾巴算;末了入口坐进 rax、单子坐进 rdi,一条 jmp 递出执行流">两扇门,和最后一跳</ChapterLink>
  <ChapterLink num="8" href="08-alive-and-audit" desc="KernelEntry 的四件事:立栈、存 rdi、自清 BSS、五桩极早异常处理器;kernel 主函数的三行字、不带清单的婴儿内核、host 十件测试和 QEMU 面板逐行验收,外加验收日可选的 GDB 取证">内核出生,和验收</ChapterLink>
</ChapterNav>

内核活了,可它眼下只会打三行字、清自己的院子、在出事的早上喊一声 rip。往后的日子轮到它自己当家:内存的家底、中断的表、自己的盘驱动,还有把鞋盒从自家格式换成 ELF 的那一小刀——那些是往后各卷的活。装载链在咱们身后合上了,而从第四行字起,机器的故事由内核自己往下写。
