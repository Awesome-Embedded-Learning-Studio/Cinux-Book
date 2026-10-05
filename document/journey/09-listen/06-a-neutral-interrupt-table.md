---
title: 06 · 一张中立的中断桌
description: "十六个桩与异常桩同一个机制,IRQ 永不带错误码,单签名连分岔都省了。register_handler 只落座,enable_line 才开门。dispatch 跑完 handler 无条件应答,没人坐的线照答后丢弃。IrqLine 是词汇不是裸数字,芯片名锁在 arch,异常岛从两户扩到六户。"
chapter: 9
order: 6
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - interrupt
  - irq
---

# 一张中立的中断桌

上一卷的表上,32 号之前站的是异常的桩,咱们今天要办的是 32 到 47 的十六个号,让 IRQ 的桩住进去。IRQ 的名号,您在门房那一章已经见过:每条线自个儿的那声称呼,说的就是外设拽线敲门那一路。机制和异常桩是同一个娘:编译器的 interrupt 属性生成桩身,模板实例化把各自的向量号烤进实例。区别倒是只有一条,可它省了大钱:异常有的带错误码有的不带,签名分了两岔。而 IRQ 永远不带错误码,十六个桩共用的是一个不带错误码参数的单签名,连分岔都省了。

桩长什么样,咱们看模板的真身:

```cpp
template <unsigned int Vector>
__attribute__((interrupt)) void irq_entry([[maybe_unused]] InterruptFrame* frame) {
    cinux::interrupt::Irq::self().dispatch(
        cinux::interrupt::IrqLine{.value = Vector - kFirstIrqVector});
}
```

桩的活就一件:把自己的向量号减去 32 折算成线号交给服务。它自己不查表、不认芯片、不发应答——全是服务的活。谁坐在服务里、咱们看装桩的现场:

```cpp
void InstallIrqStubs() {
    cinux::interrupt::Irq::self().init(cinux::arch::Pic::self());
    cinux::interrupt::Irq::self().register_handler(cinux::interrupt::IrqLine{.value = kTimerLine},
                                                   heartbeat);
    install_all(std::make_integer_sequence<unsigned int, kIrqCount>{});
}
```

头一件要办的事,是把芯片注了进去。第二件落的是心跳的座位,安在了 0 号线上。末一件装的是十六个桩,挨个进了表。您留意注入的位置:它发生在 arch 层,而不是在组合根。组合根说的就是 kernel.cpp 里那个亲手把家当装配起来的 Main,心跳的注入恰恰在那儿。为什么不一样?咱们想过了才分的:tick 的擦除函数跑在主线上,跑一次就完了。irq 的擦除函数跑在中断路径上,每一拍都免不了要路过,而中断路径上的东西,得住进不开向量化的岛。注入的位置,是路径决定的。

服务自己的家在 `kernel/interrupt/irq.hpp`,咱们把它的三张脸摆出来:

```cpp
void register_handler(IrqLine line, IrqHandler handler);
void enable_line(IrqLine line);
void dispatch(IrqLine line);
```

座位与门是分家的。register_handler 干的只是落座——谁坐在哪条线上,记进十六格的座位表。enable_line 才是找芯片开门的。为什么非要分:只落座不开门,线响了没人应,声也就进不来了。只开门而不落座,声进来了没人接——白响。谁也不能单方面地发力,真要听得见的话,两下半是缺一不可的。开线的写法您已经见过了,组合根里写的正是 Irq 的 enable_line,芯片的名字再没露过面。

dispatch 是桌上的正主,咱们连着看:

```cpp
void Irq::dispatch(IrqLine line) {
    if (line.value < kIrqLineCount && seats_[line.value] != nullptr) {
        seats_[line.value]();
    }
    if (ack_ != nullptr) {
        ack_(chip_, line);
    }
}
```

跑完座位上的 handler,然后无条件应答——应答彻底内化在了服务里。驱动不发、桩不发、发的人一个都没有。为什么收得这么死:讲门房的那一章咱们就说过,忘了谢,线就从此没了声,而办完没办完是服务安排的,礼自然也就归了服务回。谁来发都有忘的时候,而收成一处之后想忘都难。没人坐的线来了呢?您看代码:座位是空的,handler 也跳过了,应答是照答的——今天对假中断和级联噪声的行为说的就是它,谢完了客,把话丢了,门倒是继续开着。

这儿有一笔考古的对照,挺有意思的。当年第一遍的时候,默认的 handler 只谢主片——它不知道谁在敲门,一律当主片的客处理。从片上的线从此永远解不开锁——实时时钟 RTC 走的是 8 号,PS/2 的鼠标走 12 号,住的全在从片上,响了也是白响的。“暂时够用”的局限,在按真实线号应答的 dispatch 里自然就消解了:线号是桩折算好了递进来的,从片的线谢两片,主片的线谢一片,各回各的礼。咱们没有专门去修它——它压根儿没获得出生的机会。

线走的是接口,咱们也给它自己的词汇:

```cpp
struct IrqLine {
    unsigned int value;  ///< Line number, 0 on the first chip upward.
};
```

中断线不是裸数字——它在 x86 上是 0 到 15 的线号、换一个平台是中断 id,语义住在类型的身上,消费面永远不递光秃秃的数过接口。Hertz 走过的路,PhysAddr 走过的路,irq 这儿是第三回了。咱们顺手把芯片的脸也收进了 concept:IrqChip,开门的 unmask、应答的 ack 两下,和 TickSource 是一个模子刻的。满足它的正是 Pic,可讲门房的那一章就说了,Pic 自己倒是不知道:设备不知道服务,而服务知道设备,握手的一刹那就在 InstallIrqStubs 的头一行。

向量 0x20 对应的是心跳——咱们把接线知识也归个档。它住进了 irq_stubs.cpp,落座的人叫 heartbeat,一个转手就喊 Tick 的 on_interrupt。中立的脸不认识“定时器”,认识它的是 arch。哪天换一颗定时器芯片——服务的脸一个字都不用动,要动的是 arch 里的几行代码。

末了咱们数一数岛。不开向量化的编译单元,上一卷在册的是两户:isr 和 exception。本卷添了四户:pic、irq_stubs,再加的是 interrupt/irq 和 time/tick。六户齐了,一切 iret 要路过的代码全在里头——向量寄存器一根都不能脏,条款上一卷立了,本卷添的是住户。

桌咱们搭好了,座位也落了。门倒是还全捂着。诸事都齐了,欠的只剩一口气。
