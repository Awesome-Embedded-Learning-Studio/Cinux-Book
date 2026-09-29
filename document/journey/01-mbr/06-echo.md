---
title: 06 · 机器开口应答了
description: "stage2 只有十六行,回声却是完整的:MBR 留下的世界里栈已在位,一条命令拉起 QEMU,一块 512 字节的跳板把整条链跑通,debugcon 落下三行字。"
chapter: 1
order: 6
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - bootloader
---

# 机器开口应答了

万事俱备——本节咱们写本站最后的十六行代码——stage2 本体,然后按下了回车、看机器应答。

## 十六行,一句都不多

`boot/stage2.cpp` 的全文,咱们一字不改端上来:

```cpp
#include "early/boot_console.hpp"

asm(".section .text.boot, \"ax\"\n"
    ".global stage2_start\n"
    "stage2_start:\n"
    "   call Stage2Main\n"
    "0: hlt\n"
    "   jmp 0b\n"
    ".text\n");

extern "C" [[noreturn]] void Stage2Main() {
    cinux::boot::serial::PutString("[stage2] echo alive\n");
    for (;;) {
        asm volatile("hlt");
    }
}
```

就这么点。一个三条指令的入口桩,一个开口说一句话、然后永远停机的 `Stage2Main`。您可能会愣一下:前面讲入口桩那会儿,MBR 的桩好歹设了段、立了栈、存了盘号、前前后后十来条指令。到了 stage2 这儿,桩就剩 call 一条正事了?段呢?栈呢?

都现成的。前面跟 BIOS 打交道、收尾数家当的时候:MBR 跳走之前,CS、DS、ES、SS 全部归了零、栈立在 `0x7C00` 向下——段和栈,MBR 已经替 stage2 铺好了,前面那套进 C++ 之前的准备、这里一份不缺、全数继承。stage2 落地的位置还是自由区、栈顶还在 `0x7C00` 往下长、互不干扰,DAP 也好、`g_boot_drive` 也好、都还在原地、您想用随时能取。所以它的桩才有资格只剩 call 一条正事:MBR 自己受 512 字节的苦,换的就是下一段代码一落地就有家可回。

`Stage2Main` 里那句话您也认得。`PutString`,跟 MBR 用的是同一份 `early/boot_console.hpp`——前面讲头文件分层时说过的按需付体积:stage2 调它,它就在 stage2.bin 里实例化了一份,MBR 的体积一两不沾。话说完了、`for` 循环里 `hlt` 到永远。本站的 stage2、任务就是应答,应答完毕了、功成身退、躺在那等下一站来扩编。

它还有一份自己的链接脚本 `boot/stage2.ld`、跟 mbr.ld 是一个模子:`. = 0x7E00` 打头、段序列、DISCARD 一模一样,只是没有 512 字节的 ASSERT 和签名——那两样是引导扇区的专利,stage2 不用向 BIOS 证明什么。基址必须是 `0x7E00`、一个字节都不能差:MBR 那边 `ljmp` 的目标、DAP 的落点,说的都是它。链接器要是按别的地址排,`call`、取数据的地址,全体指向一个 stage2 实际不在的地方。好在 ljmp 与 DAP 两处的数,同出 `layout.hpp` 的一份。链接脚本的基址不在此列——它是手抄的,您哪天动布局、得记得连它一起对。

## 按下回车

好——代码全部就位了。您在仓库根目录敲下了命令——别急着切走、头一回让一块砖开机说话,值得您盯着屏幕等完:

```bash
cmake -B build && cmake --build build --target run
```

配置、构建、链接、拼镜像、一路滚了过去,然后 QEMU 悄悄启动——咱们没开图形窗口,机器就活在您的终端里。然后就是它了。三行字印了出来:

```text
Ready to call bios
Jump to Stage 2
[stage2] echo alive
```

咱们从头一行认起、`Ready to call bios`、MBR 说的、位置在 `int $0x13` 之前:桩已执行、世界已铺平、C++ 已开工、DAP 已备好、马上请 BIOS 读盘——`Jump to Stage 2`,还是 MBR 说的:读盘已成功、错误码检查已通过、马上远跳。第三行 `[stage2] echo alive`、换了嗓门——它是 stage2 的 `Stage2Main` 在说话。它的到场,证明了一整条链:BIOS 确实把 MBR 放进了 `0x7C00`,MBR 的 C++ 在实模式里活了下来,它跟 BIOS 的那次交道、参数、约定、内存布局、全对。读回来的 2048 字节,真的是 stage2 的代码,一字不差躺在了 `0x7E00`,`ljmp` 跳对了地方,而落地的世界——段、栈、约定——照样好使。咱们一路搭的每一环,都在这一行里点了名。

> 从按电源到听见回声、跑的每一行代码都是咱们自己写的。大伙头一次跑通的时候、多看两眼、不丢人、哈哈。

三行之后、机器安静了下来——它没有死、是在 `hlt`:CPU 睡过去,下一个中断来了醒一下、醒完转回循环接着睡。时钟的滴答一直在响、可没有任何代码会领着它走出循环。您想让机器退场、Ctrl-C 掉就行——QEMU 不会自己退,咱们也没让它退,机器停在应答完的状态、挺好。

## run 背后

收尾之前咱们把 `run` 目标本身看一眼。它比看上去的更有内容:

```cmake
add_custom_target(run
    COMMAND qemu-system-x86_64
            -drive format=raw,file=${CMAKE_CURRENT_BINARY_DIR}/cinux.img
            -display none
            -debugcon stdio
            -global isa-debugcon.iobase=0xe9
            -no-reboot
    DEPENDS boot_image
)
```

`-drive format=raw` 告诉 QEMU:这块文件您就当一块裸磁盘、别费心猜格式。`-display none`、不开窗口、咱们机器的全部输出走文字通道。中间两行是一对:`-debugcon stdio` 把调试控制台接到您的标准输入输出上,`-global isa-debugcon.iobase=0xe9` 把它焊在 0xE9 端口——代码里 `outb` 的目的地、就是这里。咱们本章念叨了一路的"写 0xE9 就能在终端看见"——落到实处,就是这两行的参数。`-no-reboot` 是最后一句体面:引导代码要是闯了大祸、CPU 会三重故障,三重故障触发了机器复位、QEMU 默认会陪着重启,于是"崩了重启、重启又崩"无限循环,咱们只能对着黑终端发呆。加上它、复位请求一来 QEMU 直接退出、案发画面留在了屏幕上。上一站开场聊过的"这个 CPU 怎么疯狂死机和重启了",在咱们的 QEMU 里,被一面旗摁住了。

您品一品 run 的形态:一条命令下去,该构建的构建,该拼的拼,该启动的启动,而屏幕上留给咱们的、只有那三行证词。上一站的验证习惯——改一行、一条命令、几秒钟见分晓——在最荒凉的引导阶段,被咱们原样保住了。变的只是命令背后的世界:那边,是您电脑上跑的测试程序。这边是一台真刀真枪从砖启动的虚拟机。

## 咱们拿到了什么

三行字还在终端里躺着、本站的家当,其实都在里面了。仓库多了 `boot/` 目录,`cmake/` 里也添了新家当。一台 QEMU 机器从一块 1MB 的裸盘上,由咱们的代码引导着活了过来。512 字节的 MBR、是一个 `.cpp`。段、栈、BIOS 的交割,每一环咱们都亲手搭过、也亲手验证过。至于那些容易无声无息吞掉一个晚上的事故,代码超标的、ASSERT 在链接期拦,bin 丢了的,make 会替咱们重新生成——今晚咱们可以睡了。

还有一样是白捡的:往后引导链条每多走一步,咱们都在关键节点留一句 `PutString`、机器不会说话,可咱们教会了它在每个关口喊一嗓子。哪一站它哑了,咱们顺着最后一声喊叫往回找、案发现场就在附近——这盏灯从今天起一直点着。

下一站咱们要让 stage2 在灯下开工干真活了:探一探机器到底有多少内存、把结果记下来,给将来接管机器的内核备好家底——它会越长越大。好在 4 个扇区的预算和那道闸门、都在等着它。
