---
title: 03 · 调试现场、验证与下一站
---

# 调试现场、验证与下一站

## 调试现场

这一段本该放真实的踩坑笔记,但 010 这个 tag 留下的 notes 是空的。好在源码注释里藏着一句非常实在的话,我们直接拿来用——main 在 `int $3` 之前有这么一句提醒:

> Note: do NOT enable interrupts (sti) yet -- we have no IRQ handlers and a pending PIT timer would cause a Double Fault via unhandled IRQ.

翻译过来就是:现在千万别 sti。我们还没有任何 IRQ handler,可一旦 sti,PIT 定时器的中断就会到来,没人接它,结果就是 Double Fault,直接重启。

这句话重要,不只因为它本身是个坑——它正好点明了这一章的边界。010 装的是"异常"安全网,接的是 CPU 自己抛的 #BP、#PF;可"硬件中断"(时钟、键盘)是另一回事,那需要 8259 PIC 和一套 IRQ handler,是下一个 tag 的活。所以这里不 sti 不是疏忽,是刻意的:在 IRQ 体系建好之前,sti 就是自找麻烦。

## 验证

`make run-big-kernel-test` 会在 QEMU 里把这条链从头验到尾:它检查 `int $3` 之后内核是不是真的活着继续、连续触发多次异常状态会不会腐坏、还有 gate 编码 `0x8E`/`0xEF` 算得对不对。

手动看效果,`make run` 的串口大概是这个样子:

```text
[BIG] GDT loaded.
[BIG] IDT loaded.
[BIG] Triggering int $3 breakpoint...

==== EXCEPTION: #BP (vector 3) ====
  RIP   = 0x...        CS  = 0x0008
  RFLAGS= 0x...
  ...
[EXCEPTION] Breakpoint at RIP=0x...
[EXCEPTION] Continuing...
[BIG] Breakpoint returned, continuing.
```

最后那行 `Breakpoint returned, continuing.` 就是我们要的点:异常被接住了,现场 dump 出来了,内核还活着。

## 下一站

到这里,big kernel 第一次有了扛事的能力——CPU 抛异常,我们能接住、能看到现场、能活着回来。但你大概也注意到了,全程没碰 sti,没碰任何硬件中断。这意味着内核现在还是"聋"的:听不到时钟、听不到键盘,外部世界发生什么它都不知道。

打破这个静默的,是下一站 [004 · PIC、IRQ 与 PIT](../004/):配上 8259 PIC、挂上 PIT 定时器、写好 IRQ handler,然后才敢 `sti`。到那时,main 里那句"现在不能 sti"的警告,才终于可以解禁。

---

### 参考

- Intel SDM Vol.3A — §6.10 + Figure 6-2/6-8(IDT gate descriptor)、Table 6-1(异常向量分配)、Figure 6-11 / Table 35-14(#PF error code)、CR2 说明。interrupt gate=0xE、trap gate=0xF 及其对 IF 的处理均为 SDM 明文标准。
- OSDev — Interrupt Descriptor Table、Exceptions。
- 本 tag 源码:[idt.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/idt.hpp)、[idt.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/idt.cpp)、[interrupts.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/interrupts.S)、[exception_handlers.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/exception_handlers.cpp)、[main.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/main.cpp);测试 [test_gdt_idt.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_gdt_idt.cpp)。
