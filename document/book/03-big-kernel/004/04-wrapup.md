---
title: 04 · 调试现场、验证与下一站
---

# 调试现场、验证与下一站

## 调试现场

011 这个 tag 没留下专门的 notes,但这套中断链里有两个真实且高频的坑,值得在这一章就讲透,因为它们会在后面的每个中断驱动 tag 里反复找上门。

最经典的一个,是 **EOI 漏发**——中断驱动开发里那种「不报错,但系统不动」的典型。你在 handler 里忘写 `PIC::send_eoi(0)`,编译照样过,`make run` 也照样启动,串口甚至会打出第一行 `uptime: 1s`——然后就没有然后了。时间冻在了 1 秒。原因前面讲过:PIC 锁住了 IRQ0 这条线,等你发 EOI,你一直不发,它就一直锁。怎么定位?看到「第一行正常、之后死寂」这个症状,第一反应就该是 EOI。用 QEMU 的 `-d int` 或者数你的 handler 被调用了几次,如果只被调用一次就再没进去,基本就是 EOI 的锅。这也是为什么 `PIT::irq0_handler` 把 `send_eoi` 写在最后一行、而不是开头——宁可让这一拍的打印独占 PIC,也不能漏。

另一个潜伏的坑,是 **remap 做一半**。有人图省事只 remap 了主片、忘了从片,或者 ICW3 的级联位写错。在只有一个 IRQ0 的本 tag 里,这种错误可能完全不暴露——因为从片上一条线都没响。它会潜伏到你接上第一个从片设备(比如键盘 IRQ1 还在主片,但鼠标 IRQ12 在从片)时才爆炸:从片的中断要么进不来,要么进来后因为只发了一半 EOI 卡死。教训是:remap 要么主从一起做对,要么别做;这章的测试里专门有 `ICW3 master cascade is 0x04` / `slave identity is 2` 两条断言,就是为了把这两个魔数焊死,别手滑。

还有一个不那么坑、但会让人困惑的点,正好呼应前面那段 `init()` 的措辞问题:你以为 `PIC::init()` 之后所有中断都关了,于是直接 `sti`,结果……其实在本 tag 也确实没出事,因为 IRQ0 还被 `unmask(0)` 单独管着。但如果你哪天直接 `sti` 而忘了 `unmask`,或者反过来,行为就会很微妙。记住模型是「两道闸」:PIC 的 IMR 一道,CPU 的 IF 一道,`init()` 只动 ICW 不动闸,闸归 `mask`/`unmask` 和 `sti`/`cli` 管。模型清晰了,调试就不晕。

## 验证

最直接的验证是跑 production big kernel,看串口:

```bash
cmake --build build --target run
```

预期串口长这样,`uptime` 每秒递增、永不停止:

```text
[BIG] Big kernel running @ 0x1000000
[BIG] GDT loaded.
[BIG] IDT loaded.
[BIG] PIC initialised.
[IRQ] Registering IRQ handlers (0x20-0x2F)...
[IRQ] All IRQ handlers registered.
[PIT] Initialised at 100 Hz (divisor=11931)
[BIG] Triggering int $3 breakpoint...
[BIG] Breakpoint returned, continuing.
[BIG] IRQ0 unmasked, enabling interrupts...
[BIG] Interrupts enabled. Entering idle loop.
[TICK] uptime: 1s
[TICK] uptime: 2s
[TICK] uptime: 3s
...
```

注意中间那行 `Breakpoint returned, continuing.`——它是 `int $3` 的复测,证明异常网在新中断体系下仍活着。看到它、再看到 `[TICK]` 开始稳定递增,这章就算点亮了。

想跑自动化行为测试,用带测试钩子的 kernel:

```bash
cmake --build build --target run-big-kernel-test
```

它在 QEMU 里验证 tick 是否真的递增、uptime 是否单调、mask 是否真能把 IRQ0 抑制住。纯逻辑(端口常量、ICW 位、EOI 该发给谁、除数算得对不对)还有一组 host 侧单测,不依赖 QEMU:

```bash
ctest --test-dir build -R 'pic|pit' --output-on-failure
```

## 下一站

到这里,内核第一次有了「时间感」——PIT 每 10 毫秒敲它一次门,它接得住、数得清、还能活着继续睡。可你大概也发现了一个尴尬:我们装了一整套硬件中断体系,IRQ1 到 IRQ15 却全是那个只发 EOI 然后丢掉的 default handler。内核听得见时钟,却依然听不见键盘、听不见串口敲进来的每一个字符。

打破这个局限,是接下来几个 tag 的活。最先被接上真 handler 的,会是串口——毕竟我们现在所有诊断信息都靠它吐,让它能「收」而不只是「发」,是后面调试一切用户态交互的前提。那一路要解决的可不只是 IRQ4 上挂个函数那么简单,但那就是下一站 [005](../005/) 的故事了。

---

### 参考

- OSDev — [8259 PIC](https://wiki.osdev.org/8259_PIC):remap 到 0x20/0x28 的标准做法、ICW1-4 序列、ICW3 级联约定、EOI 规则、spurious IRQ7/15。本章的 remap/cascade/EOI 全部以此为准。
- OSDev — [Programmable Interval Timer](https://wiki.osdev.org/Programmable_Interval_Timer):channel 0 接 IRQ0、mode 3 方波、base 1193182 Hz、除数 = base/freq、I/O 口 0x40-0x43。
- OSDev — IO port / io_wait:向 port 0x80 写字节制造约 1μs 延时,满足 8259A 对连续 I/O 写的时序要求。
- 本 tag 源码:[pic.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/pic.hpp)、[pic.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/pic.cpp)、[pit.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/pit.hpp)、[pit.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/pit.cpp)、[irq_handlers.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/irq_handlers.cpp)、[interrupts.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/interrupts.S)、[main.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/main.cpp);测试 [test_pic.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_pic.cpp)、[test_pit.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_pit.cpp)、[test_pic_pit.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_pic_pit.cpp)。
