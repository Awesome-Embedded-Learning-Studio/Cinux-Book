---
title: 03 · IRQ 路由与 PIT:让时钟嘀嗒起来
---

# IRQ 路由与 PIT:让时钟嘀嗒起来

### IRQ 路由表:data-driven 注册 0x20-0x2F

IDT 在 010 已经建好了,现在要往 0x20-0x2F 这 16 个 gate 里塞 handler。和 010 处理异常一个思路,这里也用一张表代替 16 段重复:

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

注意 `kIRQAttr` 算出来正好是 `0x8E`——和 010 异常那套是一个套路:present 位 `0x80` | 内核态 DPL `0x00` | Interrupt gate 类型 `0x0E`。这里特意用 Interrupt gate(类型位 `0x0E`),而不是 010 给 #BP 的 Trap gate(类型位 `0x0F`)。差别还是 IF 那一位:中断门进 handler 时自动清 IF,意味着处理这条 IRQ 期间不再被别的可屏蔽中断打断,这对硬件中断是想要的——你总不希望时钟中断还能被自己嵌套打断。所有 IRQ gate 都是内核态(DPL=0),第五个参数 `ist=0` 表示不用独立中断栈,复用当前栈。

汇编侧这一打 stub,复用的正是 010 那两个宏里的 `ISR_NOERRCODE`,新加 16 行:

```asm
ISR_NOERRCODE irq0_stub,  pit_irq0_handler     /* IRQ0(0x20): PIT Timer */
ISR_NOERRCODE irq1_stub,  irq_default_handler   /* IRQ1(0x21): Keyboard */
/* ... irq2-irq15 全部指向 irq_default_handler ... */
```

宏的逻辑上一篇讲过:压个假 error code 凑齐布局、保存通用寄存器、把栈指针当 `frame` 传给 C handler、回来后恢复、`iretq`。IRQ 不带硬件 error code,所以 16 个全是 `ISR_NOERRCODE`。

### 默认 handler:为什么只发 master EOI 就够(暂时)

`irq_handlers.cpp` 里,除 IRQ0 外的 15 条线全指向同一个 `irq_default_handler`:

```cpp
void irq_default_handler(InterruptFrame* /*frame*/) {
    PIC::send_eoi(0);   // 只给主片发 EOI,然后把中断丢掉
}
```

这个 handler 干的事很诚实:我不知道你是谁,但我得让你闭嘴,不然你赖着不走,后面别的中断全卡死。它故意只发主片 EOI——因为我们没法从这单个共享 handler 里知道到底是哪条线响了,干脆一律按主片处理。

这是个「暂时够用」的设计。本 tag 真正会被触发的只有 IRQ0(在主片上),其余 14 条线此时根本没有设备驱动去触发它们(键盘、串口驱动是后面的事)。default handler 的存在,纯粹是给「万一哪条线意外响了」兜个底,保证不会因为一条没人接的 IRQ 把系统推进 Double Fault。

但它有个明确的局限:如果响的是从片上的线(IRQ8-15),只发主片 EOI 解不开从片的锁。真到了键盘、串口、磁盘这些从片中断成批上来的时候,这个 default handler 就不够用了——到时候每条线会有自己的真 handler,按真实 IRQ 号发正确的双 EOI。这个边界我们记着,但不在这一章解决。

### PIT:用 0x36 命令字把时钟接上 IRQ0

8254 PIT 的 channel 0 出厂就连在 IRQ0 上,我们要做的是告诉它「以多快的频率嘀嗒」。一块命令字 + 一个 16 位除数搞定:

```cpp
void PIT::init(uint32_t freq_hz) {
    freq_hz_ = freq_hz;
    uint32_t divisor = PitHW::BASE_FREQ / freq_hz;   // 1193182 / 100 = 11931
    if (divisor > 65535) divisor = 65535;            // 除数只有 16 位,钳到范围
    if (divisor == 0) divisor = 1;

    io_outb(PitHW::COMMAND,
            PitHW::CMD_CHANNEL_0 | PitHW::CMD_LSB_MSB |
            PitHW::CMD_MODE_3    | PitHW::CMD_BINARY);   // 0x00|0x30|0x06|0x00 = 0x36
    io_outb(PitHW::CHANNEL_0, divisor & 0xFF);           // 低字节
    io_outb(PitHW::CHANNEL_0, (divisor >> 8) & 0xFF);    // 高字节
    tick_count_ = 0;
}
```

PIT 的输入时钟是固定的 1.193182 MHz,我们想要多快的中断,就除以多少:100 Hz 意味着每秒 100 次中断,除数 `1193182 / 100 = 11931`。除数只有 16 位,所以频率被框死在大约 18 Hz(除数 65535)到 1.19 MHz(除数 1)之间,代码里做了钳位。

命令字 `0x36` 是几个位域拼出来的,值得拆开记:`0x00` 选 channel 0,`0x30` 是「先写低字节再写高字节」(所以后面两次 `io_outb` 的顺序不能反),`0x06` 选 mode 3 方波发生器,`0x00` 用二进制计数。mode 3 是定时节流最常用的模式,它让 channel 0 的输出在高低之间来回跳,每次跳变都触发一次 IRQ0——比 mode 2 的「终值中断」更平滑。这几个位域凑成 `0x36`,是 PIT 编程的「口诀」,几乎每个 OS 都这么配 channel 0。

handler 这边,每来一次 IRQ0 计一次 tick,攒够一秒打一行,然后**自己**发 EOI:

```cpp
void PIT::irq0_handler(InterruptFrame* /*frame*/) {
    tick_count_++;
    if ((tick_count_ % freq_hz_) == 0)                       // 每 100 tick = 1 秒
        kprintf("[TICK] uptime: %us\n", tick_count_ / freq_hz_);
    PIC::send_eoi(0);                                         // 必须发,否则下一个 tick 永远不来
}
```

`send_eoi(0)` 放在最后,确保打印这一秒的日志时,PIC 还锁着,不会被自己嵌套打断——又一个手动 EOI 的好处。

### 串起来:main 的 9 步,以及 sti 终于解禁

最后看 main 把这些点成一条线。顺序是死的,错一步要么不嘀嗒、要么直接重启:

```cpp
cinux::lib::kprintf_init();        // ① 串口,先能说话
cinux::arch::g_gdt.init();         // ② GDT,段选好
cinux::arch::g_idt.init();         // ③ IDT,异常 gate 先就位
PIC::init();                       // ④ PIC remap,把 IRQ 搬出异常区
irq_init();                        // ⑤ 往 IDT 塞 IRQ0-15 的 gate
PIT::init(100);                    // ⑥ 配 channel 0,100 Hz
__asm__ volatile("int $3");        // ⑦ 复测:异常网在新中断体系下仍正常
PIC::unmask(0);                    // ⑧ 打开 IRQ0 这道闸
__asm__ volatile("sti");           // ⑨ 打开 CPU 中断总闸
while (1) { __asm__ volatile("hlt"); }   // idle:开中断停机,等 IRQ0 唤醒
```

为什么是这个顺序,每一步都有理由。④ 必须在 ⑤ 之前:PIC 没 remap,IRQ 注册进 IDT 的向量号就是错的,到时候 IRQ0 会以 INT 0x08 的身份进来,撞上 #DF 直接重启——这正是开头说的那个坑。⑤ 必须在 ⑨ 之前:gate 没注册就 sti,第一个 IRQ0 找不到 handler,Double Fault。⑥ 要在 ⑨ 之前,但和 ⑤ 的相对顺序其实可以换——PIT 先配好、gate 还没注册也不会出事,因为还没 sti,中断进不来。⑦ 那个 `int $3` 放在开中断之前,是刻意选的位置:确认装了 PIC、IRQ 这一堆之后,老的异常路径没被搞坏。

⑧ 和 ⑨ 这两步是「两道闸」的体现:`unmask(0)` 开 PIC 的闸,`sti` 开 CPU 的闸,少任何一个,IRQ0 都到不了 handler。最后那个 idle loop 从 010 的 `cli; hlt` 变成了单纯的 `hlt`——区别巨大。`cli; hlt` 是「关中断再停机」,CPU 永远不会被中断叫醒,纯粹等死;而 `hlt` 配合前面那句 `sti`,是「开着中断停机」,CPU 睡着等下一个 IRQ0 把它唤醒,处理完再睡回去。这就是一个事件驱动的 idle 循环的雏形。
