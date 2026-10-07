---
title: 08 · 回音,和验收
description: "环形队列留一空位判满,满丢新,单核无锁的边界摆明处说。attach 两下接上 1 号线,应答内化,当年按一下就哑的那种死法生不出来了。0xD2 注入把物理按键变自动回归,monitor 逐键喂出串口尾行 hi,截屏第 8 行 51 颗亮像素。面板八行逐字对数,run_gui 亲手打字,门的故事交给下一卷。"
chapter: 10
order: 8
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - keyboard
  - testing
---

# 回音,和验收

两半的活各自齐了——还差一根线缝上。立的是仓库,接的是线,开机听的是回音,咱们按这个次序来。

咱们把队列安在 base 的 container,起的名字叫 RingQueue,base 的家底又添一件,注释里把脾气写明白了:对并发零观点。判满的法子是留一空位:tail 追到 head 前一格就是满、俩数相等就是空——满和空,一次比较就分了家、不用另养一个计数器跟人对表。满了怎么办,push 交的是 false,新来的丢,在住的一个不动:键打得太快、消费者跟不上的时候,丢的是最新一笔,排好队的旧字一个不丢。早进来的早出去,次序靠 head 和 tail 各走各的圆圈维持:

```cpp
    bool push(Element value) {
        unsigned int const kNext = advance(head_);
        if (kNext == tail_) {
            return false;
        }
        slots_[head_] = value;
        head_         = kNext;
        return true;
    }
```

键盘用它盛的是字符事件,开的容量是 64,名义上的 64 格,一格是永远空着的,真正盛得下的是 63 个。63 个没取的键,您两只手按不满。无锁的这件事,咱们不当默认,而是当边界来讨论:单核、中断不重入。生产者是 IRQ1 的路径、消费者是主线,同一个核上住的俩人永远不可能同时在场——中断一来,主线停在原地,中断办完了就走人、才轮到它。咱们一把锁都没用上,这句话成立的条件就两条:单核、中断不重入。条件一变它就作废:等第二个核进门的那天,带锁的活归消费者自己。队列是不持观点的,观点由用的人持有——写明白了、不把侥幸当通则。

然后咱们接线。上一卷立的中断桌就是现成的接线柱,attach 的功夫就两下:

```cpp
void Keyboard::attach() {
    cinux::interrupt::Irq::self().register_handler(
        cinux::interrupt::IrqLine{.value = kKeyboardIrqLineValue}, irq1_thunk);
    cinux::interrupt::Irq::self().enable_line(
        cinux::interrupt::IrqLine{.value = kKeyboardIrqLineValue});
}
```

落了座,开了门,座位与门的分家是上一卷立的,咱们这儿原样沿用。中断的路径短得很:thunk 读一口数据、喂给 on_byte、翻一码、推一队、整数三步、没有打印没有等待——路径越短越好,上一卷立下的准则。应答呢?驱动不发。当年漏谢 EOI 的下场,是按一下就哑:线响一回就断了流,机器活得好好的,键没了。这一遍应答内化在中断桌的 dispatch 里,驱动连谢的义务都没有,想忘都没处忘——按一下就哑的病,从结构上就生不出来了。

组合根的次序,咱们看 kernel.cpp 的尾段:

```cpp
    cinux::driver::Keyboard& keyboard = cinux::driver::Keyboard::self();
    if (!keyboard.init()) {
        Println("[kern] keyboard self-test failed");
        cinux::arch::Halt();
    }
    keyboard.attach();
    asm volatile("sti" : : : "memory");
    Println("[kern] irq on, tick %uHz", cinux::time::kTickHz.value);
    Println("[kern] keyboard on, type to echo");
    for (;;) {
        asm volatile("hlt" : : : "memory");
        while (keyboard.poll()) {
            PutChar(keyboard.take());
        }
    }
```

键盘的带电排在 sti 之前——六步礼数要在安静里走完,总闸开着去改芯片的配置,它偏偏半路敲一次门,次序就全乱了。attach 也排在 sti 的之前:座位没落好就开总闸,声进来了没人接、白响一声。而后 sti,而后面板的第八行,而后是那个循环:hlt 睡着、中断叫醒,醒了把队列掏空,一笔一笔地 PutChar。两半的桥,就是循环里的那一句 PutChar(keyboard.take())——键进来、字上屏,这个字串口和画面是同时收到的。上一卷卷尾咱们许过话,说 1 号线开口说的就是人话,今天到了兑现的日子:键盘上敲 hi,两张嘴上都有了 hi。

手指头按的键,怎么进自动回归?咱们得把 0xD2 请出来。QEMU 的 8042 认一条调试命令:往命令口写 0xD2、再往数据口写一个字节,QEMU 就假装键盘发了这个字节——输出缓冲一满,IRQ1 的线照抬,从中断桌到翻译机到队列、整条路一根线不缺地真走一遍。机内测试的键盘台拿它当子弹,立了三桩:单键出的是 'a',shift 抬升出的是 'A',三键连击的次序不乱。注入的助手住在测试件里,不进产品的脸——它是验货的枪,不是货架上的货。这一次开机走到键盘那一级阶梯:中断的五步在,键盘的带电在,sti 也开了才跑用例,零点二七秒就收了工。

机外还有一条喂键的路:QEMU 的 monitor 里 sendkey,host 这头逐键地递进去,走的也是完整的 PS/2 路径。咱们实弹打过一笔:逐键喂 h、i、空格、回车,串口的尾行原文就是 `hi `。画面那头同时截了屏,按十六个像素一行的文本行去数亮像素:第 0 到 7 行是面板那八行、每行几百到一千多颗,各有各的数,第 8 行 51 颗——h 和 i 上了屏。空格是一颗不亮的,它的字模本来就全空。回车也是不亮的,它只把光标送去了第 9 行。再往后行行都归了零。第三条路是您自己的手:cmake --build build --target run_gui,弹的是一个真窗口,键盘是直通的,串口照走的是 stdio——亲手打字、亲眼看字上屏,这一卷的收束值得这么验一回。

咱们又到了对数的日子。面板的 kern 部分,原样地抄来:

```text
[kern] 64-bit C++ world alive
[kern] gdt+idt self-owned, sse on
[kern] bootinfo: 8 e820 entries, fb 1024*768*32
[kern] screen console 128 cols 48 rows
[kern] kernel at 200000 size 86778 entry 200580
[kern] pmm: 32345 pages free (probe 100000 ok)
[kern] irq on, tick 100Hz
[kern] keyboard on, type to echo
```

八行里的新面孔有两张,screen console 和 keyboard on 各占的是一行。老行里变的也只有数:咱们把内核 size 一栏的 0x86778 换算成十进制 550776 个字节,合的是 135 页,pmm 的空闲就是 32480 减 135,得的是 32345,对上了。盘上的 kernel.bin 是 21936 个字节,上一卷还是 14336——涨的七千六里,四千一是那盒字模的,其余是两半的驱动和表。长凳的十九件里,本卷新添的是四件:font 的九桩,console 的九桩,framebuffer 的四桩,keyboard 的十桩。机内的三台:selfcheck、tick、keyboard。ctest --test-dir build/test 这么一跑,22 件全都过了。镜像还是原来的 1MiB、一格没动。

验收路上咱们还闹过一回乌龙,值得记下来:有一回全量构建绿了,起机行为却整个不对——查了半天行为,末了发现盘上的镜像根本没重新生成。镜像的目标不在默认构建里,cmake --build build 造的是内核,而不是镜像。诊断的正道,是把镜像的时间戳排在一切行为分析的前头:头一件事是确认跑的是新货,然后才轮到问机器为什么怪。教程里给的入口一直是 --target run 和 --target run_gui,它俩都挂着镜像的依赖,走的都是正门,这口气就省了。

咱们的考古箱里,本卷的两位前辈是一天写的。当年的四月二十、单日九站,其中挨着的两站就是这一卷的题目:一张是画面、一副是键盘。留下的笔记一句没有,好在旧章就是实录——四段调试实录原样躺在那儿:漏谢 EOI 的哑,bit6 没开的乱,0xE0 的丢弃,时序的丢命令。这一卷咱们把它们各自安顿在了机制出现的地方——病在哪儿犯,教训就写在哪儿等着您,读到那儿的时候,它正好是醒着的。当年一天写完的东西,今天走了一整卷,每一记当年的疼,都有了自家的门牌。

门的故事还欠着一笔尾数:这一卷给显存开的是 1GiB 的粗门,整进整出的做派——豪爽,精细是谈不上的。下一卷就轮到内存当家了:虚拟内存的管理用 4KiB 的页把门重新精修一遍,堆和地址空间的活也都在那一卷等着。机器眼下安顿在开着中断的睡眠里,拍子还是照跳的,耳朵也是支着的——您敲一笔,它回你一笔。它有了自己的脸,也用上了自己的耳朵。
