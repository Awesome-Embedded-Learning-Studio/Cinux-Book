---
title: 06 · 老芯片的六步礼数
description: "键盘背后是 8042:数据口 0x60,命令口 0x64,状态也从 0x64 读。发命令前等 INPUT_FULL 清零,读数据等 OUTPUT_FULL 亮起,等待都有超时兜底。关两口、冲刷、读改写 config、自检等 0x55、开口,六步一步不能乱。bit6 是 set 2 到 set 1 的翻译开关,当年没开它,字符全乱——乱得有纪律,查起来没线索。"
chapter: 10
order: 6
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - keyboard
  - ps2
---

# 老芯片的六步礼数

键盘不是一根线直通 CPU 的。它背后住的是一位门房——8042 键盘控制器、资格比 PC 本身还老。它管的是两个口:数据口 0x60、命令口 0x64,状态也是从 0x64 读的。跟这么老的芯片打交道,讲究礼数:次序错了它不理您,错了还不喊,病全憋在它的肚子里。

礼数的第一条在时序,咱们头一个讲它,因为后头的每一句话都用得上。老芯片消化命令是要时间的,寄存器不是即时收发的:发命令之前,得看状态口的 bit1——行话 INPUT_FULL,它亮着、就说明肚里的上一条还没消化完,等它清了零再写。等回话看的是 bit0、行话 OUTPUT_FULL,亮了,数据口里才有了货,这时候的读才有意义。咱们把它包成 send_command 和 read_data 两个小函数,每一次等待都有次数上限兜底——芯片真死了,内核立马就转身走了,不陪它站到天荒地老。当年这一块咱们真吃过亏:发命令不等清零,命令偶尔就丢了、错得毫无规律,查起来的样子像闹鬼。老芯片不跟您吵,它就是不给你办。

带电的六步,咱们把 init 原样摆出来:

```cpp
bool Keyboard::init() {
    send_command(kCommandDisableFirst);
    send_command(kCommandDisableSecond);
    flush_output();
    send_command(kCommandReadConfig);
    auto const kConfig = read_data();
    auto const kWanted = static_cast<uint8_t>((kConfig | kConfigIrqFirst | kConfigTranslation) &
                                              static_cast<uint8_t>(~kConfigIrqSecond));
    send_command(kCommandWriteConfig);
    write_data(kWanted);
    send_command(kCommandSelfTest);
    if (read_data() != kCommandExpectHealthy) {
        return false;
    }
    send_command(kCommandEnableFirst);
    return true;
}
```

咱们一步一步看。头两步关门:0xAD 关的是第一口、键盘那个口。0xA7 关的是第二口,历史上接鼠标的那个口。改配置之前的礼数,是把门口的客人请出去,免得改到一半的时候有人敲门。第三步冲刷:输出缓冲里可能还躺着上电以来的旧字节——不冲干净,头一句读到的话就是别人的陈年旧话。第四步办的是读改写 config:0x20 读出来,改的是三笔、0x60 写回去。三笔里头的头一笔,置的是 bit0,开 IRQ1 的线。再一笔清的是 bit1,第二口的线不开,咱们没有鼠标,别让一条没主人的线也去敲门。末一笔置的是 bit6,开的是翻译,它的官司咱们马上单说。第五步的自检:0xAA 发出去、芯片自问自答,咱们等 0x55。答不上来的话,init 交的是 false,组合根打一行 keyboard self-test failed 就地停机——宁可承认聋,也不装作听得见的样子。第六步的 0xAE,把键盘的那道口重新开开、请客人回来。

bit6 的官司,当年咱们结结实实打过一场,咱们把病状记在这儿,因为它是带电序里最隐蔽的一位。键盘本上来说的码是 set 2,咱们的码表是 set 1——bit6 一置,8042 就替咱们把 set 2 翻成 set 1 再递进门。当年咱们没置它——症状是字符全乱:按 h,上屏的不是 h。乱得还特别有纪律——两套码表里同一个字节指着不同的键、翻译照常工作,只是翻的是另一本词典。查这一类的错,打印出来的命令字节全对,单步的走读也没毛病,病灶藏在一个默认不开的位里。咱们给它单独立了常量,名字起的就叫 translation。

为什么咱们不干脆把表换成 set 2,省掉这趟翻译?下一节您看一眼 set 1 的语法就明白了:按下和松开,差的就是一位。这样的便宜,值得咱们劳烦控制器跑一趟翻译。

照的还是旧家法,咱们来报个到。Keyboard 是 Meyers 成员单例的住户,Pmm、Pic、Tick 之后又添的一户。端口一律走 io 那层的词汇,用的是 WaitBits 加 OutB,裸的端口写不出现在这一层。连 0x60、0xAD 这些数都住在 .cpp 的匿名空间里——它们是这颗芯片的家事,不进公告的脸。

电带上了,口开了,自检也答了 0x55——从这一刻起,数据口里会源源不断地进字节——可那是裸的扫描码,而不是字。下一节咱们立翻译机。
