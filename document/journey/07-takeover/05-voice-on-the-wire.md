---
title: 05 · 换嗓子,开机
description: "-serial stdio 接棒,debugcon 那对参数退役。第一次开机,[lm] 之后是一段无声的官司:字符对、OUT 在跑、LSR 也应答,线上却没有一个字节——病根在鞋盒短报了 12 个字节,摆渡这个老实人把谎话原样办成了现实。"
chapter: 7
order: 5
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - serial
  - bootloader
---

# 换嗓子,开机

代码两头咱们都换好了,轮到 QEMU 的命令。咱们眼下的 run 目标是这副样子:

```cmake
add_custom_target(run
    COMMAND qemu-system-x86_64
            -accel kvm
            -drive format=raw,file=${CMAKE_CURRENT_BINARY_DIR}/cinux.img
            -display none
            -serial stdio
            -no-reboot
    DEPENDS boot_image
)
```

您拿它跟当年第一条 run 命令一对,变动的就是两笔。一笔在中间:`-debugcon stdio` 和 `-global isa-debugcon.iobase=0xe9` 退役了,换上来的是一行 `-serial stdio`,把 COM1 接到咱们的标准输入输出上。另一笔是添了面新旗 `-accel kvm`:有了它,QEMU 直接拿 CPU 的硬件虚拟化指令带机器,快慢上把纯软件模拟远远地甩开,行为也更贴近真正的机器——您手头的机器要是没有 KVM,它就大张旗鼓地宣告失败,咱们不悄悄降级去跑慢悠悠的模拟。代码写的是 0x3F8,终端上就有了字,中间再没有那个 QEMU 专属的外设,咱们的代码从此不往 0xE9 写任何东西,后门就算正式关张了。其余几面旗咱们都是老相识,不重叙了。

换好的命令跑起来,是一条从头到尾的单流。stage2、pm、lm、kern 整条链的世界全在 COM1 上说话,咱们一个终端从头看到尾。要是走双流的方案,boot 的话进一条道、内核的话进另一条,真正的机器上 boot 从此成了哑巴——单流是咱们挑的。boot 少说话的那点损失,上一节已经用 MBR 的取舍交代清了,换来的,就是这么一种简单:“谁说话都从同一个口子出来”。

好了,开机吧。咱们敲下 run,盯着终端——boot 的话一行一行地来了。stack ok、A20 ok、八条图谱、画布的参数,装载的两行、告别实模式,咱们一路看到 pm 报到、lm 报到。然后呢?然后没有了。内核该打的那几行,咱们一个字都没等到。机器安安静静地停在 lm 的身后,像歌手张了嘴,而音箱却没有声。

咱们请 GDB 上。断点下在内核的 SerialPutChar:进来了,字符是对的。THRE 等过去了——没卡。OUT 指令一条一条地执行,LSR 读回来的是 0x60,THRE 是置位的。怎么看都对。驱动在勤勤恳恳地发字节,可咱们线上一根毛都没见到。您体会一下那会儿的处境:所有的证词都说“在发”,所有的观察都说“没到”。这是调试里最拧巴的一种局面,证据链的每一环都干净,可拼在一起就对不上了。

咱们把怀疑从驱动挪开,回头去核鞋盒上的数。头里声明的 file_size 是 0xE90,可 objcopy 剥出来的 kernel.bin,实长 0xE9C——头把文件报短了 12 个字节。短从哪儿来?上一卷咱们看链接脚本的时候,取尾那行是把各段的 SIZEOF 一格一格加出来的,加法只认每段自己的身长。可链接器排段的时候要按 16 字节对齐,.text 的结尾到 .rodata 的开头之间,垫着一条 12 字节的缝——缝也躺在文件里,而加法却不认它。数是没算平的,头就把整个文件的长度报短了 12。

摆渡呢?摆渡是老实人。上一卷讲四段流水线的时候咱们看过它:末一窗只搬 remaining 那么多,而一个字节也不多带。鞋盒报了多少,它就分毫不差地照搬——报短了 12,它就真的少搬 12。被裁掉的是哪一段?裁的是文件末尾:镜像尾巴上那 12 个字节没有过河,内存里那一截躺着的,还是上电以来的零。您猜这 12 个字节是谁?恰好是七张条子的最后三张:清 DLAB 选 8N1 的 Lcr、配 FIFO 的 Fcr、配握手线的 Mcr——三个四字节的对子,合起来的数不多不少,恰好就是 12 个字节的身长。

接下来的连锁,咱们慢慢捋。boot 那头的 early 拷贝是全须全尾的,所以 stage2、pm、lm 的话都说出来了。坏就坏在内核进了场之后,它照自家脸面的礼数,把自家的 SerialInit 又跑了一遍,手里的表却断了尾巴:头四张条子好好的,Ier 关了,DLAB 开了,除数写了。后头的三张,读到的是三个 port=0、value=0 的空条子,发给 0 号端口的三个零,静悄悄地谁也没吵一声。可 LCR 停在了 0x80——DLAB 开着,再没有谁来关门了。此后内核的每一个字符写进 0x3F8,按 16550 的规格,DLAB 开着的时候 0 号偏移就是除数锁存器,字符一个不落全进了分频器,而一个都没上发送线。QEMU 是一点毛病都没有的,它按规格把这个写送进了分频器,发送路径根本没被叫醒——是咱们把字节递错了窗口。这也就解开了 GDB 那头全部的“正常”:发送保持寄存器的门口从头到尾没进过货,门里一直是空的,THRE 当然恒置位。“准备好收”倒是真的,只是收的从来不是要发的字。那笔 LSR=0x60 的读数,是全场最大的烟雾弹。

所以修法分两步,咱们一步一步说。取尾的算术改了章程——不再累加段长,改问的是“最后一个非空文件段的尾巴在哪儿”这么一句:

```text
__file_end_lma = SIZEOF(.data) ? LOADADDR(.data) + SIZEOF(.data) :
                (SIZEOF(.rodata) ? LOADADDR(.rodata) + SIZEOF(.rodata) :
                 LOADADDR(.text) + SIZEOF(.text));
```

谁的段里有内容,尾巴就落在谁家段的末尾,中间垫的对齐缝天然被盖住。光改还是不够的,咱们又加了两道护栏。构建期 objcopy 剥完了就核对,头里声明的 file_size 跟剥出来的实长对不上,构建当场就失败了。咱们再加一件 kernel_image 测试,拿真实链接出来的镜像把取尾的算术关进测试里。鞋盒上的 file_size,是摆渡照着办事的数,链接脚本的自述算术,是要盖住每一条对齐缝的。

咱们再开一次机。这回面板全亮了:

```text
[stage2] stack ok
A20 ok
E820: 8 entries
  #00 base=0000000000000000 len=000000000009FC00 type=1
  #01 base=000000000009FC00 len=00000000000400 type=2
  #02 base=00000000000F0000 len=0000000000010000 type=2
  #03 base=0000000000100000 len=0000000007EE0000 type=1
  #04 base=0000000007FE0000 len=0000000000020000 type=2
  #05 base=00000000FEFFC000 len=0000000000004000 type=2
  #06 base=00000000FFFC0000 len=0000000000040000 type=2
  #07 base=000000FD00000000 len=0000000300000000 type=2
VESA: VBE 0x0300 93 modes
VESA: 0x4144 1024*768*32 LFB
  fb 00000000FD000000 pitch 4096
[stage2] kernel image ok
[stage2] kernel ferried
[stage2] leaving real mode
[pm] 32-bit world alive
[lm] 64-bit world alive
[kern] 64-bit C++ world alive
[kern] bootinfo: 8 e820 entries, fb 1024*768*32
[kern] kernel at 200000 size 810C0 entry 200296
[kern] pmm: 32350 pages free (probe 100000 ok)
```

从 stack ok 到 pmm 的这一路,咱们看到的是一条流、一张嘴、一个终端。头一行是 stage2 打的——MBR 不再说话,这笔开销上一节交割过了。最末那行 pmm 是本卷的第二道菜,它的面纱,咱们下一节亲手揭。

末了说一句真正的机器。`-serial stdio` 接的口子,在真正的开发板上,就是您面前的那根串口线:您的终端和内核,就隔着那根线、说着一档波特率的话。QEMU 模拟的是它,开发板上焊的也是它——从这一卷起,咱们说的话,真正的机器也听得见了。
