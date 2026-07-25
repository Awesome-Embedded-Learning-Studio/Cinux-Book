---
title: 02 · PIC:把 16 条硬件中断挪出异常区
---

# PIC:把 16 条硬件中断挪出异常区

驱动封装成一个 `PIC` 类,全是静态方法——因为系统里就这一对芯片,没必要造实例。核心是 `init()`,它按 8259A datasheet 规定的 ICW1-ICW4 序列,把两片 PIC 拨到我们想要的状态:

```cpp
void PIC::init(uint8_t master_offset, uint8_t slave_offset) {
    master_offset_ = master_offset;   // 默认 0x20,存下来给 send_eoi 用
    slave_offset_  = slave_offset;    // 默认 0x28

    uint8_t master_mask = io_inb(PicPort::MASTER_DATA);  // 先存旧 mask
    uint8_t slave_mask  = io_inb(PicPort::SLAVE_DATA);

    // ICW1:开始初始化,级联模式,需要 ICW4
    io_outb(PicPort::MASTER_CMD, PicICW::ICW1_INIT | PicICW::ICW1_ICW4);
    io_wait();
    io_outb(PicPort::SLAVE_CMD,  PicICW::ICW1_INIT | PicICW::ICW1_ICW4);
    io_wait();

    // ICW2:向量偏移——这就是「remap」本体
    io_outb(PicPort::MASTER_DATA, master_offset);   // IRQ0-7 → 0x20-
    io_wait();
    io_outb(PicPort::SLAVE_DATA,  slave_offset);    // IRQ8-15 → 0x28-
    io_wait();

    // ICW3:级联接线
    io_outb(PicPort::MASTER_DATA, 0x04);   // 主片:IRQ2 上挂了从片
    io_wait();
    io_outb(PicPort::SLAVE_DATA,  0x02);   // 从片:我的级联身份是 2
    io_wait();

    // ICW4:8086 模式,不用 auto-EOI
    io_outb(PicPort::MASTER_DATA, PicICW::ICW4_8086);
    io_wait();
    io_outb(PicPort::SLAVE_DATA,  PicICW::ICW4_8086);
    io_wait();

    io_outb(PicPort::MASTER_DATA, master_mask);      // 恢复 mask(不带 io_wait)
    io_outb(PicPort::SLAVE_DATA,  slave_mask);
}
```

四个 ICW 各司其职,顺序不能乱:ICW1 喊一声「要初始化了」,写到命令口;紧接着写数据口的字节,PIC 自动当作 ICW2、ICW3、ICW4 收下。ICW2 的 `master_offset`/`slave_offset` 就是我们要搬去的新家地址——写 `0x20`,IRQ0 就翻译成 INT 0x20。ICW3 的 `0x04`/`0x02` 是级联的接线约定:主片 bit2 置 1 表示「我的 IRQ2 接着一片从片」,从片写 `2` 表示「我是从片,挂在主片第 2 号线上」。这两个数是 PC 平台雷打不动的约定,几乎所有 OS 教程的 remap 都是 `0x04`/`0x02`。

这里有个容易被注释带偏的细节,值得停下来讲。`pic.hpp` 的文档说「After init, all IRQs are masked」——读完会以为 `init()` 顺手把所有中断关了。但你看实现,它干的是「存旧 mask → 配 ICW → 把旧 mask 写回去」。也就是说,`init()` **不改变**哪些 IRQ 被屏蔽,它把屏蔽状态原样保留了。真正决定开不开某条线的,是后面那几次 `mask`/`unmask` 调用。措辞和实现的这点出入,记在心里,等下看 main 的时候就明白为什么那里要单独 `PIC::unmask(0)` 了。

### io_wait:为什么 ICW 之间要插一脚 0x80

上面每两条 `io_outb` 之间都夹了一个 `io_wait()`。它的实现朴素到让人怀疑人生:

```cpp
inline void io_wait() {
    io_outb(0x80, 0);   // 向 port 0x80 写个字节,纯为了「浪费时间」
}
```

port 0x80 是主板上的一个诊断口,写它没有任何有意义的副作用,但它要花掉大约 1 微秒的 I/O 周期。8259A datasheet 要求对同一片 PIC 连续写命令时,两次写之间得留够时间,老式 ISA 总线上芯片反应慢,写太快会丢字节。真实硬件上这步不能省;QEMU 上其实无所谓,但写上是正确的习惯,免得哪天搬到真机上调到怀疑人生。

### send_eoi:slave 中断为什么要发两枪

EOI(End-Of-Interrupt)是这章最容易踩、也最该讲透的概念。PIC 收到一个中断后会「锁住」这条线,不再投递同优先级及更低的中断,**直到你告诉它「我处理完了」**。这个「告诉」的动作就是发 EOI:往命令口写 `0x20`。

漏发 EOI 的后果很直接:PIC 一直锁着,下一个中断永远来不了。对只有一个 IRQ0 的本 tag 来说,现象就是 `uptime` 停在某一秒再也不涨——系统没死,但时间冻住了。要是再多几条中断互相牵扯,锁住的优先级会越来越高,最后没人能进来,看门狗或者连续未处理中断就会把系统推进 Double Fault。

`send_eoi` 的关键在级联:

```cpp
void PIC::send_eoi(uint8_t irq) {
    if (irq >= 8) {                 // 来自从片的中断
        io_outb(PicPort::SLAVE_CMD, 0x20);   // 先给从片发 EOI
    }
    io_outb(PicPort::MASTER_CMD, 0x20);      // 总是给主片发 EOI
}
```

为什么从片的中断要发两次?回想设计图:从片的中断是通过主片的 IRQ2 这条级联线传上来的。所以一次从片中断,主片那边其实是「IRQ2 收到了信号」。你只给从片发 EOI,从片是放开了,但主片还以为 IRQ2 没处理完,照样锁着,下一个从片中断还是进不来。所以规则是:**先给从片发,再给主片发**,两个都解锁,链路才通。参数传的是硬件 IRQ 号(0-15),不是 INT 向量号,`irq >= 8` 一刀切开 master/slave,干净利落。

我们刻意**没用** auto-EOI(ICW4 里有 `ICW4_AUTO_EOI` 这个位)。auto-EOI 让 PIC 在中断一被接受就自动 EOI,省一行代码,但代价是你失去了对「什么时候算处理完」的控制——handler 还没跑完,PIC 就已经放行下一个同优先级中断了,重入风险全压到你自己头上。手动 EOI 麻烦一点,但什么时候放开完全由 handler 说了算,可控得多。

### mask/unmask:一张 IMR 位图的读改写

开关节点的开关在 IMR(Interrupt Mask Register)里,每个 bit 对应一条 IRQ:置 1 屏蔽,清 0 放行。`mask`/`unmask` 都是「读出来、改一个 bit、写回去」:

```cpp
void PIC::unmask(uint8_t irq) {
    uint16_t port = (irq < 8) ? PicPort::MASTER_DATA : PicPort::SLAVE_DATA;
    uint8_t  bit  = (irq < 8) ? irq : (irq - 8);
    io_outb(port, io_inb(port) & ~(1u << bit));   // 清 bit = 放行
}
```

从片的 IRQ 要减 8 才是它在从片 IMR 里的位号。`disable_all()` 最简单粗暴,直接往两个数据口各写 `0xFF`,全屏蔽。注意 `unmask` 只是打开 PIC 这一道闸门,中断能不能真到 CPU,还得看 CPU 自己的 IF 标志——也就是后面的 `sti`。两道闸,缺一不可。
