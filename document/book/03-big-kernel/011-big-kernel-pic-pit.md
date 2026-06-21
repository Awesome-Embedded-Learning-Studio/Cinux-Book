---
title: 011 · PIC、IRQ 与 PIT：让内核听见时钟
---

# 011 · PIC、IRQ 与 PIT：让内核听见时钟

> 上一篇 010 里，我们给内核装上了异常安全网，`int $3` 能被接住、能 dump 现场、能活着回来。可当时 main 里有一句很扎眼的警告——「现在千万别 sti，我们还没有任何 IRQ handler，PIT 的中断一来就会 Double Fault」。这一章，我们把这句警告兑现：配上 8259A 双 PIC、挂上 PIT 定时器、注册好 IRQ handler，然后才敢按下 `sti`。内核从此第一次拥有「时间感」。

## 这一章我们要点亮什么

落到一个看得见的效果上：big kernel 跑起来后，串口开始每秒稳定吐一行——

```text
[TICK] uptime: 1s
[TICK] uptime: 2s
[TICK] uptime: 3s
```

不是 `kprintf` 死循环打出来的，而是真正的硬件中断：PIT 每秒触发 100 次 IRQ0，第 100、200、300……次各打一行。内核的执行流被一个外部信号反复「打断」又「接住」，而它全程没崩。这对一个裸奔内核来说，是从「只能被自己主动 `int`」跨到「能响应外部世界」的质变。

顺带我们还要确认一件事：上一章的异常网没坏。所以在开中断之前，main 会再 `int $3` 一次，证明 IDT、PIC、IRQ 这套新东西装上去之后，老的异常处理照样能正常返回。

## 为什么现在需要它

010 的内核是「聋」的。它听得见 CPU 自己抛的异常（#BP、#PF 这类），但听不见任何硬件——没有时钟、没有键盘、没有串口输入。更要命的是，它主动拒绝听见：main 末尾的 idle loop 是 `cli; hlt`，把中断整个关死，然后停机。这是一种诚实的自保，因为那时候哪怕只开一条 IRQ，都会立刻 Double Fault。

为什么一开就崩？因为 x86 的硬件中断向量默认会撞进 CPU 的异常区。8259 PIC 上电默认把 IRQ0-15 映射到 INT 8-15（0x08-0x0F），而 8-15 这段正好是 `#DF`、`#TS`、`#GP`、`#PF` 这些异常的家。PIT 一旦开始嘀嗒，IRQ0 就被当成 INT 0x08 推给 CPU，CPU 一看「这是 Double Fault 的向量」，可又没有真正的 #DF 语义去匹配，直接三重故障重启。

所以这章的真正主题不是「加个定时器」，而是「先把 16 条硬件中断从异常区里搬出来」。这个搬迁动作叫 **PIC remap**，是整章的地基。搬完之后，IRQ0-15 落到 0x20-0x2F 这段没人用的向量上，再去配 PIT、开中断，才安全。

## 设计图

先看硬件长什么样。PC 上是两片 8259A 级联，主片接 CPU，从片通过主片的 IRQ2 接上来，一共 15 条可用 IRQ 线（IRQ2 被级联占了）：

```text
                 ┌──────────────────────┐
   CPU INTR ◄────┤  Master PIC          │  IRQ0  PIT 计时器
                 │  cmd  0x20 / data 0x21│  IRQ1  键盘
                 │                       │  IRQ2 ──┐ 级联线
                 │                       │  IRQ3-7 │
                 └───────────────────────┘         │
                                                   ▼
                                   ┌──────────────────────┐
                                   │  Slave PIC           │  IRQ8   RTC
                                   │  cmd  0xA0/data 0xA1 │  IRQ9-15
                                   └──────────────────────┘
```

remap 之后，IRQ 号和 CPU 看到的中断向量号不再是同一个东西。我们要让硬件 IRQ 经过 PIC 翻译，落到 0x20 起的这一段：

```text
  硬件 IRQ        PIC 翻译后 CPU 看到的 INT 向量
  ─────────       ────────────────────────────────
  IRQ0  (PIT)  →  INT 0x20      ┐
  IRQ1  (键盘) →  INT 0x21      │  Master 片
  IRQ2  (级联) →  INT 0x22      │  IRQ0-7 → 0x20-0x27
  ...                           ┘
  IRQ8  (RTC)  →  INT 0x28      ┐
  ...                           │  Slave 片
  IRQ15        →  INT 0x2F      ┘  IRQ8-15 → 0x28-0x2F
```

一次 IRQ0 从触发到返回的完整链路，是这章所有代码要串起来的东西：

```text
  PIT 计数到 0 ──► 拉 IRQ0
                       │
                  Master PIC ──► 向 CPU 发 INT 0x20
                                     │
                          CPU 压栈(SS/RSP/RFLAGS/CS/RIP)，跳 IDT[0x20]
                                     │
                          irq0_stub(汇编)：保存通用寄存器，传 frame，call pit_irq0_handler
                                     │
                          PIT::irq0_handler：tick++，每秒打一行，PIC::send_eoi(0)
                                     │
                          stub 恢复寄存器 ──► iretq ──► 回到被打断处
```

注意链路最后那段 `send_eoi(0)`——它是这条链能不能持续运转的关键，下面专门讲。

## 代码路线

### PIC：把 16 条硬件中断挪出异常区

驱动封装成一个 `PIC` 类，全是静态方法——因为系统里就这一对芯片，没必要造实例。核心是 `init()`，它按 8259A datasheet 规定的 ICW1-ICW4 序列，把两片 PIC 拨到我们想要的状态：

```cpp
void PIC::init(uint8_t master_offset, uint8_t slave_offset) {
    master_offset_ = master_offset;   // 默认 0x20，存下来给 send_eoi 用
    slave_offset_  = slave_offset;    // 默认 0x28

    uint8_t master_mask = io_inb(PicPort::MASTER_DATA);  // 先存旧 mask
    uint8_t slave_mask  = io_inb(PicPort::SLAVE_DATA);

    // ICW1：开始初始化，级联模式，需要 ICW4
    io_outb(PicPort::MASTER_CMD, PicICW::ICW1_INIT | PicICW::ICW1_ICW4);
    io_wait();
    io_outb(PicPort::SLAVE_CMD,  PicICW::ICW1_INIT | PicICW::ICW1_ICW4);
    io_wait();

    // ICW2：向量偏移——这就是「remap」本体
    io_outb(PicPort::MASTER_DATA, master_offset);   // IRQ0-7 → 0x20-
    io_wait();
    io_outb(PicPort::SLAVE_DATA,  slave_offset);    // IRQ8-15 → 0x28-
    io_wait();

    // ICW3：级联接线
    io_outb(PicPort::MASTER_DATA, 0x04);   // 主片：IRQ2 上挂了从片
    io_wait();
    io_outb(PicPort::SLAVE_DATA,  0x02);   // 从片：我的级联身份是 2
    io_wait();

    // ICW4：8086 模式，不用 auto-EOI
    io_outb(PicPort::MASTER_DATA, PicICW::ICW4_8086);
    io_wait();
    io_outb(PicPort::SLAVE_DATA,  PicICW::ICW4_8086);
    io_wait();

    io_outb(PicPort::MASTER_DATA, master_mask);      // 恢复 mask（不带 io_wait）
    io_outb(PicPort::SLAVE_DATA,  slave_mask);
}
```

四个 ICW 各司其职，顺序不能乱：ICW1 喊一声「要初始化了」，写到命令口；紧接着写数据口的字节，PIC 自动当作 ICW2、ICW3、ICW4 收下。ICW2 的 `master_offset`/`slave_offset` 就是我们要搬去的新家地址——写 `0x20`，IRQ0 就翻译成 INT 0x20。ICW3 的 `0x04`/`0x02` 是级联的接线约定：主片 bit2 置 1 表示「我的 IRQ2 接着一片从片」，从片写 `2` 表示「我是从片，挂在主片第 2 号线上」。这两个数是 PC 平台雷打不动的约定，几乎所有 OS 教程的 remap 都是 `0x04`/`0x02`。

这里有个容易被注释带偏的细节，值得停下来讲。`pic.hpp` 的文档说「After init, all IRQs are masked」——读完会以为 `init()` 顺手把所有中断关了。但你看实现，它干的是「存旧 mask → 配 ICW → 把旧 mask 写回去」。也就是说，`init()` **不改变**哪些 IRQ 被屏蔽，它把屏蔽状态原样保留了。真正决定开不开某条线的，是后面那几次 `mask`/`unmask` 调用。措辞和实现的这点出入，记在心里，等下看 main 的时候就明白为什么那里要单独 `PIC::unmask(0)` 了。

### io_wait：为什么 ICW 之间要插一脚 0x80

上面每两条 `io_outb` 之间都夹了一个 `io_wait()`。它的实现朴素到让人怀疑人生：

```cpp
inline void io_wait() {
    io_outb(0x80, 0);   // 向 port 0x80 写个字节，纯为了「浪费时间」
}
```

port 0x80 是主板上的一个诊断口，写它没有任何有意义的副作用，但它要花掉大约 1 微秒的 I/O 周期。8259A datasheet 要求对同一片 PIC 连续写命令时，两次写之间得留够时间，老式 ISA 总线上芯片反应慢，写太快会丢字节。真实硬件上这步不能省；QEMU 上其实无所谓，但写上是正确的习惯，免得哪天搬到真机上调到怀疑人生。

### send_eoi：slave 中断为什么要发两枪

EOI（End-Of-Interrupt）是这章最容易踩、也最该讲透的概念。PIC 收到一个中断后会「锁住」这条线，不再投递同优先级及更低的中断，**直到你告诉它「我处理完了」**。这个「告诉」的动作就是发 EOI：往命令口写 `0x20`。

漏发 EOI 的后果很直接：PIC 一直锁着，下一个中断永远来不了。对只有一个 IRQ0 的本 tag 来说，现象就是 `uptime` 停在某一秒再也不涨——系统没死，但时间冻住了。要是再多几条中断互相牵扯，锁住的优先级会越来越高，最后没人能进来，看门狗或者连续未处理中断就会把系统推进 Double Fault。

`send_eoi` 的关键在级联：

```cpp
void PIC::send_eoi(uint8_t irq) {
    if (irq >= 8) {                 // 来自从片的中断
        io_outb(PicPort::SLAVE_CMD, 0x20);   // 先给从片发 EOI
    }
    io_outb(PicPort::MASTER_CMD, 0x20);      // 总是给主片发 EOI
}
```

为什么从片的中断要发两次？回想设计图：从片的中断是通过主片的 IRQ2 这条级联线传上来的。所以一次从片中断，主片那边其实是「IRQ2 收到了信号」。你只给从片发 EOI，从片是放开了，但主片还以为 IRQ2 没处理完，照样锁着，下一个从片中断还是进不来。所以规则是：**先给从片发，再给主片发**，两个都解锁，链路才通。参数传的是硬件 IRQ 号（0-15），不是 INT 向量号，`irq >= 8` 一刀切开 master/slave，干净利落。

我们刻意**没用** auto-EOI（ICW4 里有 `ICW4_AUTO_EOI` 这个位）。auto-EOI 让 PIC 在中断一被接受就自动 EOI，省一行代码，但代价是你失去了对「什么时候算处理完」的控制——handler 还没跑完，PIC 就已经放行下一个同优先级中断了，重入风险全压到你自己头上。手动 EOI 麻烦一点，但什么时候放开完全由 handler 说了算，可控得多。

### mask/unmask：一张 IMR 位图的读改写

开关节点的开关在 IMR（Interrupt Mask Register）里，每个 bit 对应一条 IRQ：置 1 屏蔽，清 0 放行。`mask`/`unmask` 都是「读出来、改一个 bit、写回去」：

```cpp
void PIC::unmask(uint8_t irq) {
    uint16_t port = (irq < 8) ? PicPort::MASTER_DATA : PicPort::SLAVE_DATA;
    uint8_t  bit  = (irq < 8) ? irq : (irq - 8);
    io_outb(port, io_inb(port) & ~(1u << bit));   // 清 bit = 放行
}
```

从片的 IRQ 要减 8 才是它在从片 IMR 里的位号。`disable_all()` 最简单粗暴，直接往两个数据口各写 `0xFF`，全屏蔽。注意 `unmask` 只是打开 PIC 这一道闸门，中断能不能真到 CPU，还得看 CPU 自己的 IF 标志——也就是后面的 `sti`。两道闸，缺一不可。

### IRQ 路由表：data-driven 注册 0x20-0x2F

IDT 在 010 已经建好了，现在要往 0x20-0x2F 这 16 个 gate 里塞 handler。和 010 处理异常一个思路，这里也用一张表代替 16 段重复：

```cpp
struct IRQRoute { uint8_t vector; IDT::Stub stub; };

static constexpr IRQRoute k_irq_routes[] = {
    {0x20, irq0_stub},  {0x21, irq1_stub},  /* ... */
    {0x28, irq8_stub},  {0x29, irq9_stub},  /* ... */  {0x2F, irq15_stub},
};

static constexpr uint8_t kIRQAttr = make_idt_attr(IDTPrivilege::Kernel, IDTGateType::Interrupt);

extern "C" void irq_init() {
    for (const auto& route : k_irq_routes)
        g_idt.set_handler(static_cast<ExceptionVector>(route.vector),
                          route.stub, GDT_KERNEL_CODE, kIRQAttr, 0);
}
```

注意 `kIRQAttr` 算出来正好是 `0x8E`——和 010 异常那套是一个套路：present 位 `0x80` | 内核态 DPL `0x00` | Interrupt gate 类型 `0x0E`。这里特意用 Interrupt gate（类型位 `0x0E`），而不是 010 给 #BP 的 Trap gate（类型位 `0x0F`）。差别还是 IF 那一位：中断门进 handler 时自动清 IF，意味着处理这条 IRQ 期间不再被别的可屏蔽中断打断，这对硬件中断是想要的——你总不希望时钟中断还能被自己嵌套打断。所有 IRQ gate 都是内核态（DPL=0），第五个参数 `ist=0` 表示不用独立中断栈，复用当前栈。

汇编侧这一打 stub，复用的正是 010 那两个宏里的 `ISR_NOERRCODE`，新加 16 行：

```asm
ISR_NOERRCODE irq0_stub,  pit_irq0_handler     /* IRQ0(0x20): PIT Timer */
ISR_NOERRCODE irq1_stub,  irq_default_handler   /* IRQ1(0x21): Keyboard */
/* ... irq2-irq15 全部指向 irq_default_handler ... */
```

宏的逻辑上一篇讲过：压个假 error code 凑齐布局、保存通用寄存器、把栈指针当 `frame` 传给 C handler、回来后恢复、`iretq`。IRQ 不带硬件 error code，所以 16 个全是 `ISR_NOERRCODE`。

### 默认 handler：为什么只发 master EOI 就够（暂时）

`irq_handlers.cpp` 里，除 IRQ0 外的 15 条线全指向同一个 `irq_default_handler`：

```cpp
void irq_default_handler(InterruptFrame* /*frame*/) {
    PIC::send_eoi(0);   // 只给主片发 EOI，然后把中断丢掉
}
```

这个 handler 干的事很诚实：我不知道你是谁，但我得让你闭嘴，不然你赖着不走，后面别的中断全卡死。它故意只发主片 EOI——因为我们没法从这单个共享 handler 里知道到底是哪条线响了，干脆一律按主片处理。

这是个「暂时够用」的设计。本 tag 真正会被触发的只有 IRQ0（在主片上），其余 14 条线此时根本没有设备驱动去触发它们（键盘、串口驱动是后面的事）。default handler 的存在，纯粹是给「万一哪条线意外响了」兜个底，保证不会因为一条没人接的 IRQ 把系统推进 Double Fault。

但它有个明确的局限：如果响的是从片上的线（IRQ8-15），只发主片 EOI 解不开从片的锁。真到了键盘、串口、磁盘这些从片中断成批上来的时候，这个 default handler 就不够用了——到时候每条线会有自己的真 handler，按真实 IRQ 号发正确的双 EOI。这个边界我们记着，但不在这一章解决。

### PIT：用 0x36 命令字把时钟接上 IRQ0

8254 PIT 的 channel 0 出厂就连在 IRQ0 上，我们要做的是告诉它「以多快的频率嘀嗒」。一块命令字 + 一个 16 位除数搞定：

```cpp
void PIT::init(uint32_t freq_hz) {
    freq_hz_ = freq_hz;
    uint32_t divisor = PitHW::BASE_FREQ / freq_hz;   // 1193182 / 100 = 11931
    if (divisor > 65535) divisor = 65535;            // 除数只有 16 位，钳到范围
    if (divisor == 0) divisor = 1;

    io_outb(PitHW::COMMAND,
            PitHW::CMD_CHANNEL_0 | PitHW::CMD_LSB_MSB |
            PitHW::CMD_MODE_3    | PitHW::CMD_BINARY);   // 0x00|0x30|0x06|0x00 = 0x36
    io_outb(PitHW::CHANNEL_0, divisor & 0xFF);           // 低字节
    io_outb(PitHW::CHANNEL_0, (divisor >> 8) & 0xFF);    // 高字节
    tick_count_ = 0;
}
```

PIT 的输入时钟是固定的 1.193182 MHz，我们想要多快的中断，就除以多少：100 Hz 意味着每秒 100 次中断，除数 `1193182 / 100 = 11931`。除数只有 16 位，所以频率被框死在大约 18 Hz（除数 65535）到 1.19 MHz（除数 1）之间，代码里做了钳位。

命令字 `0x36` 是几个位域拼出来的，值得拆开记：`0x00` 选 channel 0，`0x30` 是「先写低字节再写高字节」（所以后面两次 `io_outb` 的顺序不能反），`0x06` 选 mode 3 方波发生器，`0x00` 用二进制计数。mode 3 是定时节流最常用的模式，它让 channel 0 的输出在高低之间来回跳，每次跳变都触发一次 IRQ0——比 mode 2 的「终值中断」更平滑。这几个位域凑成 `0x36`，是 PIT 编程的「口诀」，几乎每个 OS 都这么配 channel 0。

handler 这边，每来一次 IRQ0 计一次 tick，攒够一秒打一行，然后**自己**发 EOI：

```cpp
void PIT::irq0_handler(InterruptFrame* /*frame*/) {
    tick_count_++;
    if ((tick_count_ % freq_hz_) == 0)                       // 每 100 tick = 1 秒
        kprintf("[TICK] uptime: %us\n", tick_count_ / freq_hz_);
    PIC::send_eoi(0);                                         // 必须发，否则下一个 tick 永远不来
}
```

`send_eoi(0)` 放在最后，确保打印这一秒的日志时，PIC 还锁着，不会被自己嵌套打断——又一个手动 EOI 的好处。

### 串起来：main 的 9 步，以及 sti 终于解禁

最后看 main 把这些点成一条线。顺序是死的，错一步要么不嘀嗒、要么直接重启：

```cpp
cinux::lib::kprintf_init();        // ① 串口，先能说话
cinux::arch::g_gdt.init();         // ② GDT，段选好
cinux::arch::g_idt.init();         // ③ IDT，异常 gate 先就位
PIC::init();                       // ④ PIC remap，把 IRQ 搬出异常区
irq_init();                        // ⑤ 往 IDT 塞 IRQ0-15 的 gate
PIT::init(100);                    // ⑥ 配 channel 0，100 Hz
__asm__ volatile("int $3");        // ⑦ 复测：异常网在新中断体系下仍正常
PIC::unmask(0);                    // ⑧ 打开 IRQ0 这道闸
__asm__ volatile("sti");           // ⑨ 打开 CPU 中断总闸
while (1) { __asm__ volatile("hlt"); }   // idle：开中断停机，等 IRQ0 唤醒
```

为什么是这个顺序，每一步都有理由。④ 必须在 ⑤ 之前：PIC 没 remap，IRQ 注册进 IDT 的向量号就是错的，到时候 IRQ0 会以 INT 0x08 的身份进来，撞上 #DF 直接重启——这正是开头说的那个坑。⑤ 必须在 ⑨ 之前：gate 没注册就 sti，第一个 IRQ0 找不到 handler，Double Fault。⑥ 要在 ⑨ 之前，但和 ⑤ 的相对顺序其实可以换——PIT 先配好、gate 还没注册也不会出事，因为还没 sti，中断进不来。⑦ 那个 `int $3` 放在开中断之前，是刻意选的位置：确认装了 PIC、IRQ 这一堆之后，老的异常路径没被搞坏。

⑧ 和 ⑨ 这两步是「两道闸」的体现：`unmask(0)` 开 PIC 的闸，`sti` 开 CPU 的闸，少任何一个，IRQ0 都到不了 handler。最后那个 idle loop 从 010 的 `cli; hlt` 变成了单纯的 `hlt`——区别巨大。`cli; hlt` 是「关中断再停机」，CPU 永远不会被中断叫醒，纯粹等死；而 `hlt` 配合前面那句 `sti`，是「开着中断停机」，CPU 睡着等下一个 IRQ0 把它唤醒，处理完再睡回去。这就是一个事件驱动的 idle 循环的雏形。

## 调试现场

011 这个 tag 没留下专门的 notes，但这套中断链里有两个真实且高频的坑，值得在这一章就讲透，因为它们会在后面的每个中断驱动 tag 里反复找上门。

最经典的一个，是 **EOI 漏发**——中断驱动开发里那种「不报错，但系统不动」的典型。你在 handler 里忘写 `PIC::send_eoi(0)`，编译照样过，`make run` 也照样启动，串口甚至会打出第一行 `uptime: 1s`——然后就没有然后了。时间冻在了 1 秒。原因前面讲过：PIC 锁住了 IRQ0 这条线，等你发 EOI，你一直不发，它就一直锁。怎么定位？看到「第一行正常、之后死寂」这个症状，第一反应就该是 EOI。用 QEMU 的 `-d int` 或者数你的 handler 被调用了几次，如果只被调用一次就再没进去，基本就是 EOI 的锅。这也是为什么 `PIT::irq0_handler` 把 `send_eoi` 写在最后一行、而不是开头——宁可让这一拍的打印独占 PIC，也不能漏。

另一个潜伏的坑，是 **remap 做一半**。有人图省事只 remap 了主片、忘了从片，或者 ICW3 的级联位写错。在只有一个 IRQ0 的本 tag 里，这种错误可能完全不暴露——因为从片上一条线都没响。它会潜伏到你接上第一个从片设备（比如键盘 IRQ1 还在主片，但鼠标 IRQ12 在从片）时才爆炸：从片的中断要么进不来，要么进来后因为只发了一半 EOI 卡死。教训是：remap 要么主从一起做对，要么别做；这章的测试里专门有 `ICW3 master cascade is 0x04` / `slave identity is 2` 两条断言，就是为了把这两个魔数焊死，别手滑。

还有一个不那么坑、但会让人困惑的点，正好呼应前面那段 `init()` 的措辞问题：你以为 `PIC::init()` 之后所有中断都关了，于是直接 `sti`，结果……其实在本 tag 也确实没出事，因为 IRQ0 还被 `unmask(0)` 单独管着。但如果你哪天直接 `sti` 而忘了 `unmask`，或者反过来，行为就会很微妙。记住模型是「两道闸」：PIC 的 IMR 一道，CPU 的 IF 一道，`init()` 只动 ICW 不动闸，闸归 `mask`/`unmask` 和 `sti`/`cli` 管。模型清晰了，调试就不晕。

## 验证

最直接的验证是跑 production big kernel，看串口：

```bash
cmake --build build --target run
```

预期串口长这样，`uptime` 每秒递增、永不停止：

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

注意中间那行 `Breakpoint returned, continuing.`——它是 `int $3` 的复测，证明异常网在新中断体系下仍活着。看到它、再看到 `[TICK]` 开始稳定递增，这章就算点亮了。

想跑自动化行为测试，用带测试钩子的 kernel：

```bash
cmake --build build --target run-big-kernel-test
```

它在 QEMU 里验证 tick 是否真的递增、uptime 是否单调、mask 是否真能把 IRQ0 抑制住。纯逻辑（端口常量、ICW 位、EOI 该发给谁、除数算得对不对）还有一组 host 侧单测，不依赖 QEMU：

```bash
ctest --test-dir build -R 'pic|pit' --output-on-failure
```

## 下一站

到这里，内核第一次有了「时间感」——PIT 每 10 毫秒敲它一次门，它接得住、数得清、还能活着继续睡。可你大概也发现了一个尴尬：我们装了一整套硬件中断体系，IRQ1 到 IRQ15 却全是那个只发 EOI 然后丢掉的 default handler。内核听得见时钟，却依然听不见键盘、听不见串口敲进来的每一个字符。

打破这个局限，是接下来几个 tag 的活。最先被接上真 handler 的，会是串口——毕竟我们现在所有诊断信息都靠它吐，让它能「收」而不只是「发」，是后面调试一切用户态交互的前提。那一路要解决的可不只是 IRQ4 上挂个函数那么简单，但那就是下一站 012 的故事了。

---

### 参考

- OSDev — [8259 PIC](https://wiki.osdev.org/8259_PIC)：remap 到 0x20/0x28 的标准做法、ICW1-4 序列、ICW3 级联约定、EOI 规则、spurious IRQ7/15。本章的 remap/cascade/EOI 全部以此为准。
- OSDev — [Programmable Interval Timer](https://wiki.osdev.org/Programmable_Interval_Timer)：channel 0 接 IRQ0、mode 3 方波、base 1193182 Hz、除数 = base/freq、I/O 口 0x40-0x43。
- OSDev — IO port / io_wait：向 port 0x80 写字节制造约 1μs 延时，满足 8259A 对连续 I/O 写的时序要求。
- 本 tag 源码：[pic.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/pic.hpp)、[pic.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/pic.cpp)、[pit.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/pit.hpp)、[pit.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/pit.cpp)、[irq_handlers.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/irq_handlers.cpp)、[interrupts.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/interrupts.S)、[main.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/main.cpp)；测试 [test_pic.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_pic.cpp)、[test_pit.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_pit.cpp)、[test_pic_pit.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_pic_pit.cpp)。
