# 011 Big Kernel PIC 重映射 + PIT 定时器 + IRQ 中断 — 通读版

**本章 git tag**：`011_big_kernel_pic_irq`，上一章 tag：`010_big_kernel_gdt_idt`

---

## 本章概览

到了 milestone 010，我们的大内核已经有了 GDT 和 IDT——CPU 异常可以被捕获和报告了。但说实话，这只是一半的故事。一个只有 CPU 异常处理能力而没有硬件中断的内核，就像一部只能拨打 110 报警但永远收不到短信的手机——它能处理"出事了"的场景，却无法响应外界的任何主动联系。PIT 定时器每秒滴答一百次、键盘敲击、磁盘读写完成——这些都是硬件通过中断线主动通知 CPU 的事件，而管理这些中断线的"交通警察"就是 PIC（Programmable Interrupt Controller）。这一章我们要做的事情就是：把 8259A PIC 芯片初始化好，把 IRQ 向量重映射到不会和 CPU 异常冲突的区间，配置 PIT 定时器让它每秒滴答一百次，然后让串口每秒打印一行 `[TICK] uptime: Ns`。当那行 tick 消息第一次出现在终端上的时候，你的内核就真正"活"过来了——它有了时间感。

本章的核心产出是四个模块。PIC 驱动（`pic.hpp` / `pic.cpp`）封装了 8259A 双芯片的初始化序列（ICW1–ICW4）、中断屏蔽/解除屏蔽、以及 EOI（End-Of-Interrupt）信号发送，把 IRQ0-7 重映射到 INT 0x20-0x27、IRQ8-15 重映射到 INT 0x28-0x2F。PIT 驱动（`pit.hpp` / `pit.cpp`）配置 Intel 8254 定时器的 Channel 0 为方波发生器模式，以 100 Hz 的频率产生 IRQ0 中断，并维护一个全局 tick 计数器和 uptime 追踪。IRQ handler 注册模块（`irq_handlers.cpp`）用数据驱动的路由表把 16 个硬件 IRQ 的 ISR stub 注册到 IDT 的向量 0x20-0x2F，同时为未配置的 IRQ 线提供默认 EOI-only handler 以防止未响应中断导致的系统挂死。最后，`interrupts.S` 新增了 16 个 IRQ stub 的宏实例化，和上一章的异常 stub 共用同一套 `ISR_NOERRCODE` 宏。

关键设计决策方面：PIC 初始化采用手动 EOI 模式而非 Auto-EOI 模式，这样做的原因是手动 EOI 给了我们在 handler 内部精确控制中断完成时机的能力，也为将来实现中断优先级和中断嵌套留出了空间；PIT 的 IRQ0 handler 内部自行调用 `PIC::send_eoi(0)` 发送 EOI，而不是在公共框架层统一发送——这看起来像是把 EOI 责任分散到了各个 handler，但实际上让每个 handler 拥有完整的控制权，在将来需要做延迟 EOI（比如中断下半部处理）的时候会更加灵活；IRQ 注册采用了和上一章异常注册相同的数据驱动路由表模式，16 条 IRQ 线通过一张 constexpr 数组统一注册，替代了 16 个重复的 `set_handler()` 调用。和 xv6 对比的话，xv6 的 PIC 初始化更加简洁——它把 ICW 序列直接写在 `picinit()` 函数里，没有 class 封装也没有 namespace 组织。Linux 早期版本（2.4 时代）的 8259A 驱动则比我们复杂得多——它需要处理各种主板的怪异硬件配置、SMP 下的 IRQ 路由、以及与 IO-APIC 的共存。我们的设计在工程性和可读性之间取了个务实的平衡——足够清晰到能教会读者 PIC 的每一个细节，又不会过度设计到看不出核心逻辑。

---

## 架构图

```
PIC / PIT / IRQ 中断处理全链路：

  PIT 硬件（8254 Channel 0）
       │
       │  每 10ms 产生一次方波上升沿
       ▼
  8259A Master PIC
       │  IRQ0 线被拉高
       │  PIC 检查 IMR（Interrupt Mask Register）
       │  若 IRQ0 未被屏蔽 → 向 CPU 发送 INT vector 0x20
       │
       ├── Master PIC: I/O port 0x20/0x21
       │     IRQ0 (PIT) ────────→ INT 0x20
       │     IRQ1 (Keyboard) ───→ INT 0x21
       │     IRQ2 (Cascade) ────→ 连接到 Slave PIC
       │     IRQ3-7 ────────────→ INT 0x23-0x27
       │
       └── Slave PIC: I/O port 0xA0/0xA1
             IRQ8 (RTC) ────────→ INT 0x28
             IRQ9-15 ───────────→ INT 0x29-0x2F
                  │
                  │  Slave 通过 Master 的 IRQ2 级联
                  ▼
       CPU 收到 INT 0x20
            │
            │  CPU 自动行为：
            │    1. 查 IDT[0x20] → 找到 irq0_stub
            │    2. 保存 RFLAGS/RIP/CS/RSP/SS 到内核栈
            │    3. Interrupt Gate → 清 IF（禁止嵌套）
            │    4. 跳转到 irq0_stub
            ▼
  ┌─ interrupts.S: ISR_NOERRCODE irq0_stub, pit_irq0_handler ──┐
  │                                                              │
  │  push $0（dummy error code）                                  │
  │  保存 15 个通用寄存器 → 栈上形成 InterruptFrame               │
  │  movq %rsp, %rdi                                             │
  │  call pit_irq0_handler → C bridge                            │
  │    → PIT::irq0_handler(frame)                                │
  │      tick_count_++                                            │
  │      if (tick_count_ % freq_hz_ == 0)                        │
  │        kprintf("[TICK] uptime: %us\n", ...)                  │
  │      PIC::send_eoi(0)                                        │
  │  恢复 15 个通用寄存器                                         │
  │  addq $8, %rsp → 弹出 dummy error code                      │
  │  iretq → 回到被中断的代码                                     │
  └──────────────────────────────────────────────────────────────┘


  初始化调用链（kernel_main）：

  kernel_main()
      │
      ├── kprintf_init()          ← 串口初始化（kprintf 基础设施）
      │
      ├── g_gdt.init()            ← GDT 必须最先（段描述符）
      │
      ├── g_idt.init()            ← IDT 依赖 GDT 的段选择子
      │
      ├── PIC::init()             ← PIC 重映射（ICW1-ICW4）
      │     │
      │     ├── ICW1: 开始初始化，级联模式，需要 ICW4
      │     ├── ICW2: Master→0x20, Slave→0x28（向量偏移）
      │     ├── ICW3: Master bit 2=1（Slave 在 IRQ2）
      │     │        Slave ID=2（级联身份编号）
      │     ├── ICW4: 8086 模式，手动 EOI
      │     └── 恢复保存的 IMR mask
      │
      ├── irq_init()              ← 注册 IRQ stub 到 IDT 0x20-0x2F
      │
      ├── PIT::init(100)          ← 配置 PIT Channel 0 @ 100 Hz
      │     │
      │     ├── 计算 divisor = 1193182 / 100 = 11931
      │     ├── 写命令字 0x36 到 port 0x43
      │     └── 写 divisor 低/高字节到 port 0x40
      │
      ├── PIC::unmask(0)          ← 解除 IRQ0 的屏蔽
      │
      ├── sti                     ← 开中断！
      │
      └── while(1) { hlt; }      ← 空闲循环，等待中断


  8259A PIC 级联拓扑：

           CPU INTR 引脚
               │
               ▼
        ┌──────────────┐
        │  Master PIC  │  I/O: 0x20 (CMD) / 0x21 (DATA)
        │              │
        │  IRQ0: PIT   │────→ INT 0x20
        │  IRQ1: KBD   │────→ INT 0x21
        │  IRQ2: ──────│────→ 级联到 Slave
        │  IRQ3: COM2  │────→ INT 0x23
        │  IRQ4: COM1  │────→ INT 0x24
        │  IRQ5: LPT2  │────→ INT 0x25
        │  IRQ6: FDD   │────→ INT 0x26
        │  IRQ7: LPT1  │────→ INT 0x27
        └──────┬───────┘
               │ IRQ2 级联线
               ▼
        ┌──────────────┐
        │  Slave PIC   │  I/O: 0xA0 (CMD) / 0xA1 (DATA)
        │              │
        │  IRQ8:  RTC  │────→ INT 0x28
        │  IRQ9:  Free │────→ INT 0x29
        │  IRQ10: Free │────→ INT 0x2A
        │  IRQ11: Free │────→ INT 0x2B
        │  IRQ12: PS2  │────→ INT 0x2C
        │  IRQ13: FPU  │────→ INT 0x2D
        │  IRQ14: ATA1 │────→ INT 0x2E
        │  IRQ15: ATA2 │────→ INT 0x2F
        └──────────────┘
```

---

## 关键代码精讲

### PIC 驱动：和 8259A 打交道的艺术

x86 PC 上有两个 8259A PIC 芯片——一个 Master、一个 Slave，通过 IRQ2 级联在一起，总共提供 16 条 IRQ 线。这个设计从 IBM PC/AT 时代一直沿用到现在，虽然现代机器早就用上了 APIC，但 QEMU 模拟的仍然是最经典的 8259A 配置，所以我们得先把它搞定。

8259A 有一个非常让人头疼的默认行为——上电之后，它把 IRQ0-7 映射到 INT 0x08-0x0F、IRQ8-15 映射到 INT 0x70-0x77。你看到问题了吧？INT 0x08-0x0F 正好和 CPU 的异常向量重叠——`#DF`（Double Fault）是向量 8，`#GP`（General Protection）是向量 13。如果不清掉这个默认映射，IRQ0 的定时器中断会伪装成一个 Double Fault，你的异常 handler 会看到一堆莫名其妙的寄存器快照然后直接 fatal halt。这就是为什么 PIC 初始化的第一步永远是重映射——把 IRQ 的向量基址挪到 0x20 以后，避开 Intel 保留的 0x00-0x1F 异常区间。

我们来看 `pic.hpp` 的设计。整个 PIC 驱被封在 `cinux::arch` namespace 下，用一组 constexpr 常量（`PicPort` 和 `PicICW`）把 8259A 的 I/O 端口和命令字参数做了语义化命名，然后用一个全是静态方法的 `PIC` class 把初始化、屏蔽、EOI 三个核心操作封装起来。之所以用全静态方法而不是实例，原因很直接——系统里只有一对 PIC 芯片，而且它们的 I/O 端口是硬编码的（Master 0x20/0x21、Slave 0xA0/0xA1），不存在需要多实例或者动态配置端口的场景。

`PicPort` namespace 里定义了四个端口地址：`MASTER_CMD = 0x20` 和 `MASTER_DATA = 0x21` 控制 Master PIC，`SLAVE_CMD = 0xA0` 和 `SLAVE_DATA = 0xA1` 控制 Slave PIC。8259A 的命令/数据端口复用机制有点微妙——当你往命令端口（CMD）写数据时，8259A 根据 ICW/OCW 命令字类型来解读你的数据；当你往数据端口（DATA）写数据时，它通常理解为中断屏蔽寄存器（IMR）的操作或者 ICW2-4 的后续初始化字。`PicICW` namespace 则把 ICW（Initialization Command Word）和 OCW（Operation Control Word）的各个位域做了命名，这样我们在写初始化序列的时候，读代码的人能直接看懂每一位的含义，而不是对着一个裸的 `0x11` 猜这是什么意思。

现在来看 `pic.cpp` 的 `PIC::init()` 实现。这个函数做的事情可以用一句话概括：往 Master 和 Slave 的 PIC 发送四组初始化命令字（ICW1-ICW4），期间每次 I/O 写入之后调用 `io_wait()` 延时约 1 微秒以满足 ISA 总线的时序要求。

函数开头先把两个 PIC 当前的 IMR mask 读出来保存——这个操作看似多余，但考虑到 BIOS 或者之前的代码可能已经设置了特定的中断屏蔽位，直接覆盖掉不太礼貌。然后进入正式的 ICW 序列。ICW1 发送到命令端口（CMD），内容是 `ICW1_INIT | ICW1_ICW4`，即 `0x10 | 0x01 = 0x11`。这个字节告诉 8259A 两件事：第一，"请进入初始化模式"（bit 4 = 1）；第二，"我会发送 ICW4"（bit 0 = 1）。注意 bit 1（`ICW1_SINGLE`）没有设——这表示我们工作在级联模式（cascade mode），即有两片 PIC，而不是单片。ICW1 必须同时发给 Master 和 Slave——两片芯片各自独立进入初始化状态。每次写入后跟一个 `io_wait()`，这个延时对真实硬件是必须的，QEMU 虽然不严格要求，但保持一致是好习惯。

ICW2 发送到数据端口（DATA），它设置了向量偏移——Master 用 `master_offset`（默认 0x20），Slave 用 `slave_offset`（默认 0x28）。这意味着 Master PIC 的 IRQ0-7 会分别触发 INT 0x20-0x27，Slave PIC 的 IRQ8-15 会触发 INT 0x28-0x2F。这两个偏移值的选择不是随意的——0x20 正好是 Intel 保留异常向量（0x00-0x1F）之后的第一个可用区间，0x28 接在 0x27 后面，中间没有间隙。这里有一个容易踩的坑：ICW2 的低 3 位必须为零（8259A 会用向量号的高 5 位加上 IRQ 线号来拼出最终的 INT 向量），所以我们传入的偏移必须是 8 的倍数。0x20 和 0x28 都满足这个约束。

ICW3 是级联配置字，Master 和 Slave 的含义不同。Master 的 ICW3 指的是"我的哪根 IRQ 线连着 Slave"——在标准 PC 配置中，Slave 连在 Master 的 IRQ2 上，所以 Master 的 ICW3 写 `0x04`（bit 2 = 1）。Slave 的 ICW3 指的是"我的级联身份编号是多少"——Slave 应答 Master 的 cascade 线时用这个编号来标识自己，标准配置写 `0x02`。这两个值看起来像是同一个数字，但语义完全不同——一个是 bitmask，一个是 ID 编号，8259A 的设计就是这么...有特色。

ICW4 是最后一个初始化字，发到数据端口，内容是 `ICW4_8086 = 0x01`。这个字节选择 8086 模式（而不是 MCS-80/85 模式——那个太古早了，我们不会用到）。注意我们没有设置 `ICW4_AUTO_EOI`（bit 1）——这意味着我们选择了手动 EOI 模式，每个中断 handler 在结束前必须显式调用 `PIC::send_eoi()`。手动 EOI 比 Auto-EOI 多了一步，但它给了我们一个非常重要的能力：在 handler 执行期间，同优先级的 IRQ 不会被提前响应，直到我们主动发送 EOI 表示"我处理完了"。这对将来实现中断优先级、中断嵌套和中断下半部（bottom-half）处理至关重要。

初始化序列的最后一步是恢复之前保存的 IMR mask。刚初始化完毕的 PIC 默认屏蔽了所有 IRQ（实际上不是默认——是我们保存的 mask 被还原了），需要调用 `PIC::unmask(irq)` 来逐条解除屏蔽。这种保守策略避免了在 IDT 还没准备好 handler 的时候就收到意外的硬件中断。

接下来看 `send_eoi()` 的实现。EOI（End-Of-Interrupt）是 x86 中断处理中最关键的概念之一——8259A 在收到一个中断请求后，会把它标记为"正在服务"（In-Service），在这个标记被清除之前，同优先级或更低优先级的中断不会被转发给 CPU。EOI 命令就是清除这个标记的方式。具体来说，往 PIC 的命令端口写入 `0x20` 就是发送 EOI——这是 8259A 的 OCW2 命令格式，bit 5（`0x20`）表示"非特定 EOI"（Non-Specific EOI），即清除当前最高优先级的 In-Service 位。

`send_eoi()` 有一个关键细节：如果 IRQ 来自 Slave PIC（IRQ 编号 >= 8），你必须同时向 Slave 和 Master 两个 PIC 都发送 EOI。原因在于级联拓扑——当一个 Slave IRQ 触发时，信号传导路径是 Slave → Master IRQ2 → CPU INTR。所以"正在服务"标记同时存在于 Slave 和 Master 的 ISR（In-Service Register）中。如果只给 Slave 发 EOI 而忘了 Master，Master 会一直认为 IRQ2 还在服务中，后续来自 Slave 的所有中断都会被阻塞。这个 bug 笔者见过不止一个人踩过——症状是"中断收到了几个就再也不来了"，非常诡异。

`mask()` 和 `unmask()` 的实现都是对 IMR（Interrupt Mask Register）的读-改-写操作。IMR 是一个 8 位寄存器，每一位对应一条 IRQ 线——bit 0 对应 IRQ0，bit 7 对应 IRQ7。置 1 表示屏蔽，清 0 表示允许。对于 Master PIC 的 IRQ0-7，直接在对应位操作；对于 Slave PIC 的 IRQ8-15，先把 IRQ 编号减 8 得到位偏移，然后在 Slave 的 IMR 上操作。`disable_all()` 则简单粗暴——直接往两个数据端口写 `0xFF`，一次屏蔽全部 16 条 IRQ 线。

### PIT 驱动：让内核拥有时间感

PIT（Programmable Interval Timer，Intel 8254）是 PC 平台上最经典的可编程定时器，它的 Channel 0 直接连接到 Master PIC 的 IRQ0 线——这意味着只要我们正确配置了 PIT 和 PIC，定时器中断就会周期性地送达 CPU，我们就有了一个"时钟"。

`pit.hpp` 首先定义了 `PitHW` namespace 来存放硬件常量。Channel 0 的数据端口是 `0x40`，命令寄存器是 `0x43`。PIT 的基准时钟频率是 `1193182 Hz`——这个看起来很随意的数字实际上是有历史原因的，它等于 1.193182 MHz，来自 NTSC 电视信号的 subcarrier 频率（3.579545 MHz）除以 3。PC 的设计师当年为了节省芯片成本，直接用电视信号的时钟分频来驱动定时器，于是这个神奇的数字就一直沿用至今。

PIT 的命令字结构是这样的：高 2 位选择通道（00 = Channel 0），中间 2 位选择访问模式（11 = LSB then MSB），再 3 位选择工作模式（011 = Square Wave Generator），最低位选择计数进制（0 = Binary）。组合起来就是 `0x00 | 0x30 | 0x06 | 0x00 = 0x36`，这正是我们在 `init()` 里写到命令寄存器的值。方波模式（Mode 3）的含义是计数器从 divisor 值开始递减，到 0 时输出一个方波的上升沿（触发 IRQ0），然后自动重新加载 divisor 继续计数——如此周而复始，产生稳定的周期性中断。

`init()` 函数首先计算分频系数（divisor）：`1193182 / freq_hz`。这个 divisor 值决定了中断频率——divisor 越大，中断频率越低；divisor 越小，中断频率越高。16 位计数器的范围是 1-65535，所以可用的频率范围大约是 18 Hz（divisor = 65535）到 1193182 Hz（divisor = 1）。我们默认用 100 Hz（divisor = 11931），这意味着每 10 毫秒一次中断——对于内核的调度和定时来说是个不错的粒度。

写 divisor 的顺序很重要——必须先写低字节到 Channel 0 的数据端口（`0x40`），再写高字节到同一个端口。8254 内部有一个缓冲逻辑，当你设置了 LSB-then-MSB 的访问模式后，第一次写入被当作低 8 位，第二次写入被当作高 8 位，自动拼成一个 16 位的计数初值。如果顺序搞反了，你的中断频率就会完全不对——比如本来应该 100 Hz 变成了不到 1 Hz，你会以为内核卡死了。

PIT class 维护两个静态变量：`tick_count_` 记录从初始化以来的总 tick 数，`freq_hz_` 记录配置的频率。`get_ticks()` 直接返回 `tick_count_`，`get_uptime_ms()` 则用 `(tick_count_ * 1000) / freq_hz_` 算出毫秒级的运行时间。这里有一个精度取舍——纯整数运算避免了浮点数（内核态不适合用浮点），但长时间运行后累积误差会逐渐增大。对于当前阶段来说这完全够用，真要高精度计时的话得用 HPET 或者 TSC（Time Stamp Counter）。

`irq0_handler()` 是整个定时器系统的核心——每次 IRQ0 到来时，ISR stub 从汇编跳到这里。它做的事情很直接：先把 `tick_count_` 加一，然后检查是不是到了一整秒——条件是 `tick_count_ % freq_hz_ == 0`，即每 `freq_hz_` 个 tick（也就是每秒）触发一次打印。打印内容是 `[TICK] uptime: Ns`，其中 N 是整数秒数。最后也是最关键的一步——调用 `PIC::send_eoi(0)` 发送 EOI。这一步绝对不能忘——如果忘了发 EOI，8259A 就会认为 IRQ0 还在服务中，后续所有同优先级或更低优先级的中断（也就是所有 16 条 IRQ 线）都不会再被转发给 CPU，整个中断系统就瘫痪了。

文件末尾有一个 `extern "C"` 的桥接函数 `pit_irq0_handler()`，它的唯一作用是把 C++ 的 `PIT::irq0_handler()` 包装成 C 链接规范的函数，这样 `interrupts.S` 里的 `call pit_irq0_handler` 才能正确链接到它。这个模式和上一章的异常处理完全一样——汇编的世界只认 C 符号名，不认 C++ 的 name mangling。

### IRQ handler 注册：数据驱动的路由表

`irq_handlers.cpp` 把 16 条 IRQ 线的 ISR stub 注册到 IDT 中，和上一章 `idt.cpp` 里的异常注册逻辑如出一辙。文件开头声明了 16 个 `extern "C"` 的 ISR stub（从 `irq0_stub` 到 `irq15_stub`），它们定义在 `interrupts.S` 中。

然后是一张 `IRQRoute` 结构体的 constexpr 数组 `k_irq_routes`，每项包含两个元素：INT 向量号和对应的 ISR stub 函数指针。16 条 IRQ 正好映射到 0x20-0x2F——Master PIC 的 IRQ0-7 对应 0x20-0x27，Slave PIC 的 IRQ8-15 对应 0x28-0x2F。这张表和上一章的异常路由表结构完全一致，只是向量范围从 0x00-0x0E 换成了 0x20-0x2F。`kIRQAttr` 是所有 IRQ entry 共用的属性字节，通过 `make_idt_attr(IDTPrivilege::Kernel, IDTGateType::Interrupt)` 生成——内核态特权级（DPL=0）加上 Interrupt Gate（会清 IF 标志禁止中断嵌套）。

接下来是 `irq_default_handler()`——这是 IRQ1-15 的统一处理函数。它做的事情简单到令人发指：直接调用 `PIC::send_eoi(0)` 发送 Master-only EOI。你可能会问，为什么只给 Master 发 EOI？因为对于来自 Slave PIC 的 IRQ（IRQ8-15），如果我们没有注册专门的 handler，说明我们不期望这些中断到来——但如果由于某种原因（比如硬件毛刺）它们还是来了，我们至少要给 Master 发 EOI 防止中断系统卡死。严格来说，对于来自 Slave 的 IRQ 应该同时给两个 PIC 都发 EOI，但由于我们目前屏蔽了所有不需要的 IRQ，`irq_default_handler` 理论上不会被触发——它只是一个安全网。等到我们真的需要 Slave 上的 IRQ（比如键盘、ATA 磁盘）时，会为它们注册专门的 handler。

`irq_init()` 函数遍历 `k_irq_routes` 数组，对每条路由调用 `g_idt.set_handler()` 注册到 IDT。这里复用了上一章 IDT class 的基础设施——`set_handler()` 把 ISR stub 地址拆成三段填入 16 字节的 IDT entry，段选择子用 `GDT_KERNEL_CODE`（0x08），IST 暂时为 0。注册完毕后，IDT 的向量 0x20-0x2F 就全部指向了对应的 IRQ stub，中断分发的硬件链路打通了。

### interrupts.S：IRQ stub 的宏实例化

`interrupts.S` 在上一章的基础上新增了 16 个 IRQ stub 的实例化。这些 stub 和异常 stub 完全共用 `ISR_NOERRCODE` 宏——因为硬件中断没有 error code，所以所有 IRQ stub 都走"push dummy 0"的路径。

16 个 stub 的命名很规律——`irq0_stub` 到 `irq15_stub`，各自绑定到一个 C handler。其中 `irq0_stub` 绑定到 `pit_irq0_handler`（我们 PIT 驱动的桥接函数），其余 15 个全部绑定到 `irq_default_handler`。每个 stub 上方都有注释标明对应的硬件设备和 INT 向量号，比如 `/* IRQ0(0x20): PIT Timer */`，方便后续扩展时快速定位需要修改的位置。

一个值得注意的点是 IRQ2（Cascade）。在标准 PC 配置中，Master PIC 的 IRQ2 被用来级联 Slave PIC——它不是一条真正的外部中断线，而是两片 PIC 之间的内部信号通道。所以 IRQ2 的 handler 永远不会被外部设备触发（除非你的硬件配置很非主流），但我们仍然给它注册了一个 default handler 作为占位——这比留一个空的 IDT entry 要安全得多，因为空的 IDT entry 在中断到来时会触发 Triple Fault。

### kernel_main：点火！

`kernel_main()` 中的初始化顺序经过精心安排，每一步都依赖于前一步的完成。串口最先初始化——因为后面每一步都需要 kprintf 输出诊断信息。GDT 第二——IDT 中的段选择子要指向 GDT 中的合法描述符。IDT 第三——CPU 异常的 handler 需要 IDT。然后是 PIC 第四——重映射 IRQ 向量，但此时 IDT 中还没有 IRQ 的 handler，所以所有 IRQ 仍然被屏蔽着不会触发。`irq_init()` 第五——在 IDT 中注册 16 个 IRQ handler。PIT 第六——配置定时器硬件开始产生方波。到这里，所有硬件和软件基础设施都已就绪，但中断仍然是关闭的——CPU 的 IF 标志还是 0。

接下来是 `PIC::unmask(0)`——解除 IRQ0（PIT 定时器）的屏蔽。这一步很关键但也很容易忘：PIC 初始化后所有 IRQ 都是屏蔽状态，如果你忘了 unmask 就直接 `sti`，PIT 的中断信号会被 PIC 拦截，CPU 永远收不到 IRQ0。unmask 之后，`__asm__ volatile("sti")` 设置 RFLAGS 的 IF 标志，CPU 开始响应可屏蔽中断。从这一刻起，每 10 毫秒就会有一次 IRQ0 到来，ISR 执行、tick 递增、每秒打印一行。

最后内核进入 `while(1) { hlt; }` 空闲循环。`hlt` 指令让 CPU 进入低功耗停机状态，直到下一个中断到来——这比纯 `while(1)` 死循环友好得多，因为它不会占满 CPU。每次中断到来时 CPU 醒来处理中断、执行 `iretq` 返回到 `hlt` 之后、然后又停机等待下一个中断——如此循环往复，内核就以一种事件驱动的方式运行着。

中间有一段有趣的测试代码——`__asm__ volatile("int $3")` 触发一个软件断点。这是上一章留下的回归测试，验证 PIC/IRQ 初始化后异常处理链路仍然正常工作。如果 PIC 重映射搞砸了，或者 IRQ stub 的 push/pop 不平衡，这个 `int $3` 就可能触发 Triple Fault 而不是正常的断点处理。看到 `Breakpoint returned, continuing.` 这行输出就意味着整个 GDT → IDT → PIC → IRQ 的链路是通的。

---

## 设计决策深度分析

### 决策一：手动 EOI vs Auto-EOI

**问题**：8259A 的 ICW4 中有一个 Auto-EOI 选项。如果启用了 Auto-EOI，PIC 会在 CPU ACK 中断（通过 INTA 序列）后自动清除 In-Service 位，不需要 handler 显式调用 `send_eoi()`。Auto-EOI 看起来更简洁——少了每 handler 一个 `send_eoi()` 调用，少了忘记 EOI 导致中断卡死的风险。但我们的设计选择了手动 EOI。

**本项目的做法**：ICW4 只设置了 `ICW4_8086`（0x01），没有设置 `ICW4_AUTO_EOI`（0x02）。每个 IRQ handler 在处理完毕后必须显式调用 `PIC::send_eoi(irq)`。PIT 的 `irq0_handler()` 在递增 tick 和打印信息之后调用 `PIC::send_eoi(0)`，`irq_default_handler()` 在入口处就调用 `PIC::send_eoi(0)` 做"立即 EOI"。

**备选方案**：在 ICW4 中设置 Auto-EOI 位（`ICW4_8086 | ICW4_AUTO_EOI = 0x03`）。这样 PIC 在 INTA 序列的第二拍就自动发送 EOI，handler 完全不需要关心 EOI。很多小型 OS 教程项目就是这样做的一一大减少了代码量和出错概率。

**为什么不选备选方案**：Auto-EOI 有一个根本性的问题——它在 handler 开始执行之前就已经清除了 In-Service 位，这意味着同优先级的中断可以在 handler 执行期间嵌套进来。设想一个场景：PIT 的 IRQ0 handler 正在执行 `kprintf`，此时下一个 IRQ0 又到了。Auto-EOI 模式下，这个新中断会立刻嵌套执行，导致 `tick_count_++` 和 `kprintf` 在嵌套上下文中运行，`tick_count_` 的更新会产生竞态条件，串口输出会乱成一锅粥。手动 EOI 模式下，In-Service 位在整个 handler 执行期间保持设置，同优先级的中断被阻塞直到 handler 主动发送 EOI——这给了我们对中断时序的完全控制权。此外，手动 EOI 是将来实现中断下半部（bottom-half）处理的基础——我们可以先做关键的上半部处理，发送 EOI 允许新中断进来，然后再执行不那么紧急的下半部工作。

**如果要扩展/改进**：当引入更多 IRQ handler（键盘、磁盘、网络）时，每个 handler 都要在返回前调用 `PIC::send_eoi()`。可以考虑封装一个 IRQ handler 基类或者 RAII wrapper，在构造时保存 IRQ 号，析构时自动发送 EOI——这样即使 handler 因为异常提前返回，EOI 也不会漏发。更进一步，当迁移到 APIC 后，EOI 机制会变成往 Local APIC 的 EOI 寄存器写 0——接口会变，但"手动 EOI"的设计思路不变。

### 决策二：PIT 100 Hz 频率的选择

**问题**：PIT 的中断频率是一个需要权衡的参数——频率越高，定时精度越好，但中断开销也越大；频率越低，中断开销小了，但定时精度下降。常见的选择有 18.2 Hz（BIOS 默认）、100 Hz（Linux 传统默认）、250 Hz（部分 Linux 配置）、1000 Hz（某些实时系统）。

**本项目的做法**：默认 100 Hz（10 ms per tick），通过 `PIT::init(100)` 配置。divisor = 1193182 / 100 = 11931，这是一个合法的 16 位值。

**备选方案**：用 1000 Hz（1 ms per tick）获得更高的定时精度，代价是每秒多 10 倍的中断开销——每次中断都要保存/恢复 15 个寄存器，跳转到 ISR stub，执行 C handler，发送 EOI。用 18.2 Hz（BIOS 默认，divisor = 65535）最小化中断开销，但定时精度粗糙到约 55 ms 一步。

**为什么不选备选方案**：100 Hz 是一个经过实战检验的平衡点——Linux 内核从诞生之初就使用 100 Hz 的时钟中断频率（直到 2.6 版本引入了可配置的 HZ 值）。对于我们的教学内核来说，100 Hz 既足够精细到能做简单的调度时间片管理（10 ms 粒度），又不会让中断开销吃掉太多的 CPU 时间。1000 Hz 在 QEMU 模拟环境中尤其不推荐——QEMU 的中断注入本身就有一点延迟，过高的频率可能导致中断堆积。18.2 Hz 虽然是 BIOS 默认值，但那是 1981 年 IBM PC 的选择——当时的 CPU 是 4.77 MHz 的 8088，100 Hz 对它来说太奢侈了。我们有的是 CPU 时间，不需要这么抠门。

**如果要扩展/改进**：如果要实现更精确的时间测量，可以用 CPU 的 TSC（Time Stamp Counter）——通过 `rdtsc` 指令读取，精度达到 CPU 时钟周期级别。TSC 适合做高精度的时间戳和性能分析，PIT 则继续作为周期性中断源。如果要支持动态 tick（tickless kernel），可以在空闲时把 PIT 配置为一次触发模式（Mode 0），只在需要下一个超时时才重新编程，避免无意义的周期性唤醒——这是现代 Linux 的 `NO_HZ` 机制的核心思想。

### 决策三：默认 EOI-only handler vs 完全不注册未使用的 IRQ

**问题**：我们有 16 条 IRQ 线，但当前只用了 IRQ0（PIT）。对于 IRQ1-15，有两种策略——要么在 IDT 中注册一个"什么都不做只发 EOI"的默认 handler，要么完全不注册（让 IDT entry 为空）。前者的好处是安全——即使意外收到中断也不会 Triple Fault；后者的好处是更"干净"——你确信不会收到的中断就没有必要注册。

**本项目的做法**：为所有 16 条 IRQ 线都注册了 handler。IRQ0 有专门的 `pit_irq0_handler`，IRQ1-15 统一使用 `irq_default_handler`（只发送 EOI）。同时 PIC 初始化后所有 IRQ 都被屏蔽，只有 IRQ0 被 unmask。

**备选方案**：只注册 IRQ0 的 handler，其余 15 个 IDT entry（0x21-0x2F）保持空。由于 PIC 已经屏蔽了 IRQ1-15，理论上它们不会触发，空 IDT entry 也不会被命中。这样做代码更少，但也更危险——如果某个时刻 PIC 的 mask 寄存器被意外篡改（比如一个有 bug 的 I/O 操作），一个未注册的 IRQ 就会导致 Triple Fault，而且你完全不知道发生了什么。

**为什么不选备选方案**：防御性编程在内核开发中不是可选项，而是生存法则。一个 EOI-only handler 的代码量几乎可以忽略——就一个 `PIC::send_eoi(0)` 调用——但它提供了一个至关重要的安全网。设想一个场景：你的某个驱动在调试时不小心往 PIC 的数据端口写了一个错误的值，意外 unmask 了 IRQ1（键盘）。如果你没有注册 IRQ1 的 handler，键盘上的每一次按键都会触发 Triple Fault。有了 EOI-only handler，最坏的情况是中断被忽略——系统继续运行，你可以在日志中看到异常行为然后排查。此外，注册全部 IRQ 的做法也是很多成熟内核的选择——Linux 的 `init_IRQ()` 就会为所有可能的 IRQ 线注册默认 handler。

**如果要扩展/改进**：可以为默认 handler 增加日志记录——当意外的 IRQ 到来时打印一条警告信息，包含 IRQ 编号和到达的次数。这有助于调试硬件配置问题。另外，当引入 APIC 后，IRQ 路由会变得动态——某些 IRQ 可能被重定向到特定的 CPU 核心，默认 handler 的设计也需要相应调整。

---

## 常见变体与扩展方向

**1. 键盘驱动（IRQ1）** ⭐

当前 IRQ1-15 都用 default handler，最自然的下一步是接上键盘。PS/2 键盘控制器的数据端口是 `0x60`，每次按键产生 IRQ1 中断，handler 通过 `io_inb(0x60)` 读取 scancode，然后解码成 ASCII 字符。这个扩展的难度不大——写一个 `Keyboard` class，注册 IRQ1 handler，unmask IRQ1，然后在 handler 里读 scancode 并 kprintf 输出。真正的挑战在于 scancode 到 key code 的映射表——PC 键盘有 Scan Code Set 1 和 Set 2 两种编码方案，扩展键（方向键、功能键）的 scancode 是多字节的。

**2. 从 8259A PIC 迁移到 IO-APIC** ⭐⭐⭐

8259A 只支持 16 条 IRQ 线，而且不支持 SMP（多核）。现代 x86 系统使用 IO-APIC（I/O Advanced Programmable Interrupt Controller），它支持 24 条以上的 IRQ 线、可编程的中断路由（把不同 IRQ 分配给不同 CPU 核心）、以及边沿/电平触发的配置。迁移到 IO-APIC 需要重写整个中断初始化逻辑——检测 IO-APIC 的存在、映射 MMIO 寄存器、配置 Redirection Table 条目、处理 EOI 寄存器。这是一个大工程，但也是理解现代 x86 中断架构的必经之路。

**3. 实现可睡眠的定时器（sleep / msleep）** ⭐⭐

当前 PIT 只做了一件事——每秒打印 uptime。但它可以作为更高级定时器服务的基础。可以维护一个定时器回调链表——每个定时器记录"到期 tick"和回调函数，`irq0_handler` 在每次 tick 时检查链表，到期就执行回调。在这个基础上可以实现 `msleep()`——让当前"线程"（目前我们还没有线程概念，所以可以是简单的忙等）等待指定的毫秒数。更高级的做法是把等待者加入一个等待队列，`msleep` 时主动 yield CPU（将来有了调度器之后）。

**4. 用 HPET 替代 PIT 作为系统时钟源** ⭐⭐

PIT 是最古老的定时器，但不是最精确的。HPET（High Precision Event Timer）提供纳秒级的计时能力，而且支持多个独立的比较器（comparator），可以同时设置多个不同频率的定时器。HPET 通过 MMIO 访问（不是 I/O 端口），需要先通过 ACPI 表定位它的物理地址。如果目标是在 QEMU 中运行，HPET 是默认模拟的，可以直接使用。

**5. 中断计数和性能监控** ⭐

可以为每条 IRQ 线维护一个中断计数器（`uint64_t irq_count[16]`），在 handler 入口处递增对应的计数器。然后提供一个命令或者 debug 接口来查询每个 IRQ 被触发的次数。这在性能分析和调试中非常有用——如果你发现 IRQ0 的计数在一秒内远超 100，说明你的 PIT 配置可能有误；如果某个设备的中断计数为 0 但你确定它在工作，说明 IRQ 可能被屏蔽了或者路由不正确。

---

## 参考资料

**Intel 手册（精确章节号）**：

- Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 3A
  - Section 8.3 — Interrupt Controller (8259A) Overview（8259A PIC 架构总览）
  - Section 8.3.1 — Initialization Command Words (ICW1-ICW4)（ICW 初始化序列的详细位域定义）
  - Section 8.3.2 — Operation Command Words (OCW1-OCW3)（IMR 屏蔽、EOI 发送、轮询模式）
  - Section 8.3.3 — End-of-Interrupt (EOI)（特定/非特定 EOI 的语义和级联 PIC 的 EOI 规则）
  - Section 8.5.4 — 8254 PIT Programming（PIT 三个通道、命令字格式、工作模式）
  - Section 6.10 — Interrupt Descriptor Table (IDT)（IDT entry 结构和 Gate 类型定义）

**AMD 手册**：

- AMD64 Architecture Programmer's Manual, Volume 2: System Programming
  - Section 14.1 — 8259A Compatible Interrupt Controller
  - Section 14.2 — Advanced Programmable Interrupt Controller (APIC)
  - Section 14.4 — I/O APIC

**OSDev Wiki**：

- [8259A PIC](https://wiki.osdev.org/8259_PIC) — PIC 重映射、ICW 序列、EOI 机制的完整教程
- [PIC](https://wiki.osdev.org/PIT) — PIT 8254 编程指南，包含所有工作模式的详解
- [Programmable Interval Timer](https://wiki.osdev.org/Programmable_Interval_Timer) — PIT 硬件寄存器、命令字和频率计算
- [IRQ](https://wiki.osdev.org/IRQ) — 标准 PC IRQ 分配表和级联拓扑
- [Interrupts](https://wiki.osdev.org/Interrupts) — x86 中断机制的总览
- [IO wait](https://wiki.osdev.org/IO_wait) — I/O 端口延时的几种实现方式

**其他参考资源**：

- James Molloy 的内核开发教程中关于 [IRQ 和 PIT](http://www.jamesmolloy.co.uk/tutorial_html/6.-IRQs%20and%20PIT.html) 的章节，对 ICW 序列有很好的逐步讲解
- OSDev Wiki 的 [Interrupt Service Routine](https://wiki.osdev.org/Interrupt_Service_Routines) 页面，描述了从硬件中断到软件 handler 的完整路径
- Linux 内核源码 `arch/x86/kernel/i8259.c`（8259A 驱动）和 `drivers/clocksource/i8253.c`（PIT 驱动），是生产级代码的参考实现
- Bran's Kernel Development Tutorial 的 [PIC remapping](https://wiki.osdev.org/Brendan%27s_Conventional_Kernel_Bare_Bones) 部分，展示了最简化的 PIC 初始化代码
