# 011 PIC 重映射 + PIT 定时器 —— 让内核拥有时间感

## 章节导语

上一章（010）我们给大内核装上了完整的异常处理基础设施——GDT、IDT、ISR stub、异常处理函数，用一条 `int $3` 验证了整条链路能正常工作。但说实话，目前这个内核还只是一个"只会接 CPU 异常电话"的系统，外部世界的任何事件它都听不到——定时器在嘀嗒、键盘在按键、磁盘在读写，CPU 对这些一无所知，因为我们根本没配置硬件中断的入口。

这一章我们要做的是把硬件中断（IRQ）真正接进来。具体来说，我们需要配置 8259A PIC（可编程中断控制器），把 IRQ0-15 重映射到 IDT 的空闲向量区域（0x20-0x2F），然后配置 PIT（可编程间隔定时器）让它每秒产生 100 次中断（100 Hz），最后写一个 IRQ0 处理函数来统计 tick 并每秒在串口打印一行 `[TICK] uptime: Ns`。完成本章后，你会看到 QEMU 串口每秒稳定地输出一行 uptick 信息——这意味着我们的内核第一次有了"时间感"，第一次能对外部硬件事件做出响应。

本章的前置知识是上一章（010_big_kernel_gdt_idt）的 GDT/IDT/ISR 基础设施。你需要理解 IDT 的 set_handler 机制、ISR stub 的工作流程、以及 `InterruptFrame` 的布局。如果你还没读完 010，建议先回去补完。

---

## 概念精讲

### 8259A PIC：x86 外部中断的"前台接待"

在 x86 架构里，CPU 并不是直接和每个外部设备打交道的。外部设备（键盘、定时器、磁盘等）产生的中断信号先到达一个叫做 PIC（Programmable Interrupt Controller，可编程中断控制器）的芯片，PIC 收到信号后做两件事：一是根据优先级决定哪个中断应该先处理（IRQ0 优先级最高，IRQ7 最低），二是把中断请求转换成一个向量号发送给 CPU。CPU 收到这个向量号之后，就去 IDT 里查对应的处理程序——这个过程和我们上一章处理 CPU 异常的流程完全一样，唯一的区别是触发源从"CPU 内部异常"变成了"外部硬件中断"。

PC 兼容机上有两片 8259A PIC 芯片，以级联（cascade）方式连接——Master PIC 负责处理 IRQ0-7，Slave PIC 负责处理 IRQ8-15，Slave 通过 Master 的 IRQ2 线路级联上去。这套设计是 IBM PC/AT 时代留下来的遗产，虽然现代系统早就用 APIC（Advanced PIC）替代了 8259A，但在 QEMU 的默认配置里 8259A 仍然存在且可用，对于教学目的来说完全够用。

```
硬件中断的传递路径：

外部设备 → IRQ 线路 → PIC → INTR 信号 → CPU → 查 IDT[向量号] → 跳转到 ISR stub → C handler

Master PIC (IRQ0-7) 的级联拓扑：
                ┌──────────────────────────────────────────┐
  PIT Timer ──→ │ IRQ0                                     │
  Keyboard ───→ │ IRQ1                                     │
                │ IRQ2 ←── Slave PIC (IRQ8-15)            │ → CPU INTR
  COM2 ──────→ │ IRQ3                                     │
  COM1 ──────→ │ IRQ4                                     │
  ...          │ IRQ5, IRQ6, IRQ7                         │
                └──────────────────────────────────────────┘

Slave PIC (IRQ8-15):
  RTC ──────→  │ IRQ8-15                                 │ → Master IRQ2
  ...
```

这里有一个非常重要的问题需要解决——BIOS 默认把 Master PIC 的 IRQ0-7 映射到 INT 0x08-0x0F，Slave PIC 的 IRQ8-15 映射到 INT 0x70-0x77。这个映射和 CPU 的异常向量号冲突了——INT 0x08 是 #DF（Double Fault），INT 0x0E 是 #PF（Page Fault），如果 IRQ 不重映射的话，定时器中断一来，CPU 以为发生了 Double Fault，直接跳到 Double Fault 处理函数去了。所以 PIC 初始化的核心任务就是把 IRQ 重映射到 IDT 里空闲的区域——习惯上用 0x20-0x2F（也就是 IDT 的 32-47 号条目），因为 Intel 保留了 0-31 给 CPU 异常，32 开始就是自由使用的。

```
PIC 重映射前后的向量号对照：

重映射前（BIOS 默认，会冲突！）：
  IRQ0 → INT 0x08  ← 和 #DF 冲突！
  IRQ1 → INT 0x09  ← 和 #DB 冲突！
  ...
  IRQ7 → INT 0x0F
  IRQ8 → INT 0x70
  ...

重映射后（本章的配置，安全）：
  IRQ0 → INT 0x20  (vector 32)
  IRQ1 → INT 0x21  (vector 33)
  ...
  IRQ7 → INT 0x27  (vector 39)
  IRQ8 → INT 0x28  (vector 40)
  ...
  IRQ15 → INT 0x2F (vector 47)
```

### ICW1-ICW4：PIC 初始化的四次"握手"

8259A 的初始化过程是一个固定的四步序列，叫做 ICW（Initialization Command Word）。每一步往 PIC 的 I/O 端口写一个字节，PIC 内部状态机按顺序接收。这四步分别是：

- **ICW1**：告诉 PIC "我要开始初始化了"，同时指明是否级联模式、是否需要 ICW4。我们写 `0x11`（INIT=1, ICW4 needed=1, cascade mode）。
- **ICW2**：设置向量号基址。Master 写 `0x20`，Slave 写 `0x28`。这一步决定了 IRQ0 会映射到 INT 0x20、IRQ1 到 INT 0x21，以此类推。
- **ICW3**：告诉两片 PIC 级联拓扑。Master 写 `0x04`（表示 IRQ2 上接了 Slave），Slave 写 `0x02`（表示自己连在 Master 的 IRQ2 线路上）。
- **ICW4**：设置工作模式。我们写 `0x01`（8086 模式，不用 auto-EOI，不用 buffered mode）。

一个很关键的细节是——8259A 的数据手册要求两次连续的 I/O 写入之间有一定的延迟（因为这是 ISA 总线时代的芯片，时序很慢）。我们在每次 `io_outb` 之后调用一次 `io_wait()`（往端口 0x80 写一个字节，大约 1 微秒的延迟），来满足这个时序要求。如果你的 PIC 初始化之后行为异常（比如该来的中断不来、EOI 发了没反应），第一个要检查的就是有没有漏掉 `io_wait()`。

### EOI：中断结束的"签收单"

8259A 有一个非常重要的机制——当它把一个中断发给 CPU 之后，它会"记住"这个中断正在被处理，在收到 EOI（End Of Interrupt）信号之前，同优先级和更低优先级的中断都不会被转发给 CPU。这意味着中断处理函数在结束之前必须显式地给 PIC 发一个 EOI，告诉它"这个中断我已经处理完了，你可以接收下一个了"。

EOI 的发送方式是往 PIC 的 Command 端口写 `0x20`。对于来自 Slave PIC（IRQ8-15）的中断，需要先给 Slave 发 EOI，再给 Master 发 EOI——因为 Master 是通过 IRQ2 收到 Slave 的级联信号的，如果只给 Slave 发了 EOI 而不给 Master 发，Master 就一直认为 IRQ2 上的中断还没处理完，后续所有中断都会被阻塞。

这个 EOI 机制是一个极其常见的坑——如果你忘了发 EOI，或者 EOI 发错了顺序，症状就是"中断只来一次就再也不来了"。这种情况很容易被误认为是 PIC 配置错误或者 IDT 注册错误，但实际原因只是忘了签收。

### PIT：x86 的"心脏起搏器"

PIT（Programmable Interval Timer，Intel 8254）是 x86 平台上的定时器芯片，它有一个固定的输入时钟频率 1193182 Hz（约 1.193 MHz，这个奇怪的数字是 PC 原始设计里 NTSC 彩色副载波频率 3.579545 MHz 除以 3 得来的），通过设置一个 16 位的除数（divisor），可以得到任意频率的中断输出：`输出频率 = 1193182 / divisor`。

PIT 有三个通道（Channel 0/1/2），其中 Channel 0（I/O 端口 0x40）直接连接到 IRQ0——也就是说，我们只要配置好 Channel 0，每到一个 tick，PIC 就会给 CPU 发一个 IRQ0 中断。Channel 1 是 RAM 刷新用的（别碰），Channel 2 连着 PC 喇叭（可以用来发蜂鸣声，但不影响中断）。

配置 Channel 0 的步骤是：先往 Command Register（端口 0x43）写命令字节 `0x36`（选择 Channel 0、LSB-then-MSB 读写模式、方波生成模式 Mode 3、二进制计数），然后把除数拆成低字节和高字节，依次写入 Channel 0 数据端口（0x40）。

```
PIT 命令字节 0x36 的位域分解：

Bit 7-6: 00 = 选择 Channel 0
Bit 5-4: 11 = LSB 然后 MSB（先写低字节再写高字节）
Bit 3-1: 110 = Mode 3（方波生成器，square wave）
Bit 0:   0 = 二进制计数（非 BCD）

最终值: 00_11_110_0 = 0x36
```

我们选择 100 Hz 的频率（除数 = 1193182 / 100 = 11931），意味着每 10 毫秒产生一次中断。这是一个比较常见的选择——Linux 内核早期也用 100 Hz（HZ=100），后来提高到 250 Hz、300 Hz 甚至 1000 Hz。频率越高计时精度越好，但中断开销也越大。对于我们这个教学内核，100 Hz 完全够用。

### AT&T 汇编语法速查

这一章涉及的新汇编指令不多，主要就是 `sti`（Set Interrupt Flag，开中断）和 `hlt`（Halt，CPU 暂停直到下一个中断到来）。如果你跟着上一章走过来的，ISR stub 的宏我们完全复用，不需要写新的汇编代码。

| 操作 | AT&T 语法 | 含义 |
|------|-----------|------|
| 开中断 | `sti` | 设置 RFLAGS.IF=1，允许 CPU 响应可屏蔽中断 |
| 关中断 | `cli` | 清除 RFLAGS.IF=0，禁止 CPU 响应可屏蔽中断 |
| CPU 暂停 | `hlt` | CPU 停止执行直到下一个中断到来 |
| 空闲循环 | `sti; hlt`（组合） | 开中断后暂停，等待下一个中断唤醒 CPU |

---

## 动手实现

### 第一步——PIC 驱动头文件：端口常量、ICW 常量、class 封装

**目标**：创建 PIC 驱动的 C++ 头文件，用命名空间常量定义 PIC 的 I/O 端口和 ICW 位域，用 class 封装 PIC 的全部操作（init、send_eoi、mask、unmask、disable_all）。

我们这一章的 PIC 设计全部用 static 方法——因为系统里只有一对 8259A PIC 芯片，不需要实例化。这和上一章 GDT 的 `g_gdt` 全局实例不太一样，PIC 没有需要存储在实例里的状态（除了两个 offset 值，它们用 static 成员变量就够了）。

**代码**（文件路径：`kernel/arch/x86_64/pic.hpp`）：

```cpp
#pragma once
#include <stdint.h>

namespace cinux::arch {

// PIC I/O Port Constants
namespace PicPort {
constexpr uint16_t MASTER_CMD  = 0x20;
constexpr uint16_t MASTER_DATA = 0x21;
constexpr uint16_t SLAVE_CMD   = 0xA0;
constexpr uint16_t SLAVE_DATA  = 0xA1;
}

// PIC ICW Constants
namespace PicICW {
constexpr uint8_t ICW1_ICW4     = 0x01;  // ICW4 needed
constexpr uint8_t ICW1_INIT     = 0x10;  // Initialization

constexpr uint8_t ICW4_8086     = 0x01;  // 8086 mode
}

class PIC {
public:
    static void init(uint8_t master_offset = 0x20,
                     uint8_t slave_offset = 0x28);
    static void send_eoi(uint8_t irq);
    static void mask(uint8_t irq);
    static void unmask(uint8_t irq);
    static void disable_all();
    static uint8_t master_offset();
    static uint8_t slave_offset();

private:
    static uint8_t master_offset_;
    static uint8_t slave_offset_;
};

}  // namespace cinux::arch
```

`PicPort` 命名空间里的四个常量是 8259A 的标准 I/O 端口地址——Master PIC 用 0x20/0x21（Command/Data），Slave PIC 用 0xA0/0xA1，这是硬件固定分配的，从 IBM PC 时代就没变过。`PicICW` 命名空间里定义了 ICW1 和 ICW4 的关键位域——我们只定义了实际用到的那几个，没有把所有可能的位都列出来，避免让头文件变成一本数据手册。

你可能注意到 `ICW1_INIT = 0x10` 和 `ICW1_ICW4 = 0x01` 是分开定义的，实际使用时用 `|` 组合成 `0x11`。这是因为 ICW1 的各个 bit 是独立的控制位，分开定义可以让调用者清楚地知道每个 bit 的含义，而不是面对一个魔法数字 `0x11` 去猜它到底设置了什么。

`PIC::init` 的两个参数 `master_offset` 和 `slave_offset` 有默认值（0x20 和 0x28），这是 x86 OS 开发中约定俗成的 IRQ 重映射基址。提供参数化接口的好处是，如果将来我们要换到不同的向量区域，只需要改调用参数就行，不需要改实现。

**验证**：此步完成后编译应该通过，但还没有可运行的输出。

### 第二步——PIC 驱动实现：ICW1-4 初始化序列、EOI、mask/unmask

**目标**：实现 PIC 的所有方法。init() 发送完整的 ICW1-ICW4 序列并附带 io_wait() 延迟；send_eoi() 处理 Master/Slave 双发逻辑；mask()/unmask() 通过 read-modify-write 操作 IMR 寄存器。

**代码**（文件路径：`kernel/arch/x86_64/pic.cpp`）：

先看 init() 函数——这是 PIC 驱动里最核心的部分：

```cpp
#include "kernel/arch/x86_64/pic.hpp"
#include <stdint.h>
#include "kernel/arch/x86_64/io.hpp"

using cinux::io::io_inb;
using cinux::io::io_outb;
using cinux::io::io_wait;

namespace cinux::arch {

uint8_t PIC::master_offset_ = 0x20;
uint8_t PIC::slave_offset_  = 0x28;

void PIC::init(uint8_t master_offset, uint8_t slave_offset) {
    master_offset_ = master_offset;
    slave_offset_  = slave_offset;

    // Save current masks
    uint8_t master_mask = io_inb(PicPort::MASTER_DATA);
    uint8_t slave_mask  = io_inb(PicPort::SLAVE_DATA);

    // ICW1: start init, cascade mode, ICW4 needed
    io_outb(PicPort::MASTER_CMD, PicICW::ICW1_INIT | PicICW::ICW1_ICW4);
    io_wait();
    io_outb(PicPort::SLAVE_CMD,  PicICW::ICW1_INIT | PicICW::ICW1_ICW4);
    io_wait();

    // ICW2: vector offsets
    io_outb(PicPort::MASTER_DATA, master_offset);
    io_wait();
    io_outb(PicPort::SLAVE_DATA,  slave_offset);
    io_wait();

    // ICW3: cascade wiring
    io_outb(PicPort::MASTER_DATA, 0x04);  // Master: slave on IRQ2
    io_wait();
    io_outb(PicPort::SLAVE_DATA,  0x02);  // Slave: cascade identity = 2
    io_wait();

    // ICW4: 8086 mode, no auto-EOI
    io_outb(PicPort::MASTER_DATA, PicICW::ICW4_8086);
    io_wait();
    io_outb(PicPort::SLAVE_DATA,  PicICW::ICW4_8086);
    io_wait();

    // Restore saved masks
    io_outb(PicPort::MASTER_DATA, master_mask);
    io_outb(PicPort::SLAVE_DATA,  slave_mask);
}
```

这段代码有几个值得仔细看的地方。首先是开头保存 mask 的操作——`io_inb` 读取当前 PIC 的 Interrupt Mask Register（IMR），保存在局部变量里。为什么要保存？因为 ICW1 的 INIT 位一写入，PIC 就进入初始化模式，接下来的三次写入（ICW2/3/4）会被 PIC 状态机依次接收。在这个过程中，IMR 被清零了（所有 IRQ 都被 unmask），但我们不希望在 init 返回之后突然所有 IRQ 都变成 enabled 的状态——那可能触发我们还没准备好处理的中断。所以在 ICW4 写完之后，我们把之前保存的 mask 值写回去，恢复原来的屏蔽状态。

ICW3 的两个值含义不同但容易混淆。Master 写 `0x04`（bit 2 置 1）意思是"我的 IRQ2 引脚上接了一个 Slave PIC"，这是一个位图——如果有多个 Slave 的话，每个 Slave 连接的 IRQ 位都要置 1。Slave 写 `0x02` 意思是"我连在 Master 的 IRQ2 上"，这是一个数值（不是位图）——Slave 只需要报告自己的级联身份编号。两个都和数字 2 有关，但语义完全不同，写反了的话级联就断了，Slave PIC 上的 IRQ8-15 全部失效。

接下来是 send_eoi()——中断处理的"签收"操作：

```cpp
void PIC::send_eoi(uint8_t irq) {
    if (irq >= 8) {
        io_outb(PicPort::SLAVE_CMD, 0x20);  // Slave PIC EOI
    }
    io_outb(PicPort::MASTER_CMD, 0x20);     // Always master EOI
}
```

EOI 的逻辑很直接——对于来自 Slave PIC 的中断（IRQ8-15），先给 Slave 发 EOI 再给 Master 发；对于来自 Master PIC 的中断（IRQ0-7），只给 Master 发就行了。注意参数是 IRQ 号（0-15），不是 INT 向量号——因为调用者（比如 PIT 的 IRQ0 处理函数）知道自己处理的是哪个 IRQ，但不需要关心它被重映射到了哪个向量号。内部通过 `irq >= 8` 判断是 Master 还是 Slave。

然后是 mask/unmask 操作，用于控制单个 IRQ 线路的开关：

```cpp
void PIC::unmask(uint8_t irq) {
    uint16_t port;
    uint8_t  value;

    if (irq < 8) {
        port  = PicPort::MASTER_DATA;
        value = io_inb(port) & ~(1u << irq);
    } else {
        port  = PicPort::SLAVE_DATA;
        value = io_inb(port) & ~(1u << (irq - 8));
    }

    io_outb(port, value);
}
```

unmask 的逻辑是"读-改-写"三步曲：先从对应的 PIC Data 端口读出当前 IMR 的值，然后把目标 IRQ 对应的 bit 清零（用 `& ~(1u << n)` 实现），再写回去。mask 操作几乎一样，只是把位操作从清零变成置位（用 `| (1u << n)` 实现）。

disable_all() 最简单——直接往两个 PIC 的 Data 端口写 0xFF，把所有 8 个 IRQ 全部屏蔽掉：

```cpp
void PIC::disable_all() {
    io_outb(PicPort::MASTER_DATA, 0xFF);
    io_outb(PicPort::SLAVE_DATA, 0xFF);
}
```

**验证**：此步完成后编译通过，但还没有可运行的输出。

### 第三步——PIT 驱动头文件：硬件常量、class 封装

**目标**：创建 PIT 驱动的 C++ 头文件，用命名空间常量定义 PIT 的 I/O 端口和命令字节位域，用 class 封装 PIT 的操作（init、irq0_handler、get_ticks、get_uptime_ms）。

**代码**（文件路径：`kernel/drivers/pit.hpp`）：

```cpp
#pragma once
#include <stdint.h>

namespace cinux::arch {
struct InterruptFrame;
}

namespace cinux::drivers {

namespace PitHW {
constexpr uint16_t CHANNEL_0    = 0x40;  // Channel 0 data port
constexpr uint16_t COMMAND      = 0x43;  // Mode/Command register
constexpr uint32_t BASE_FREQ    = 1193182;  // Input clock (Hz)

// Command byte bits
constexpr uint8_t CMD_MODE_3     = 0x06;  // Square wave generator
constexpr uint8_t CMD_LSB_MSB    = 0x30;  // LSB then MSB
constexpr uint8_t CMD_CHANNEL_0  = 0x00;  // Select channel 0
constexpr uint8_t CMD_BINARY     = 0x00;  // Binary counter mode
}

class PIT {
public:
    static void init(uint32_t freq_hz = 100);
    static void irq0_handler(cinux::arch::InterruptFrame* frame);
    static uint64_t get_ticks();
    static uint64_t get_uptime_ms();
    static uint32_t freq_hz();

private:
    static uint64_t tick_count_;
    static uint32_t freq_hz_;
};

}  // namespace cinux::drivers
```

PIT 的头文件结构和 PIC 类似——`PitHW` 命名空间放硬件常量，`PIT` class 放操作接口。`InterruptFrame` 用前向声明而不是 include，因为头文件里只需要知道这个类型存在，不需要知道它的完整定义，这样可以减少头文件依赖。

`BASE_FREQ = 1193182` 这个数字看起来很奇怪，它是 PC 原始设计的产物——8254 PIT 的输入时钟来自一个 1.193182 MHz 的晶振，这个频率是 NTSC 彩色电视信号的副载波频率（3.579545 MHz）除以 3 得到的。IBM 在设计 PC 的时候为了节省成本，直接用了现成的电视晶振来驱动定时器，这个数字就这么一直传承下来了。1193182 是一个质数的近似值，它有一个好处是可以被很多整数整除——比如除以 65536（16 位除数的最大值）得到约 18.2 Hz（BIOS 默认频率），除以 11931 得到 100 Hz（我们的选择），除以 1 得到 1.193 MHz（理论最高频率）。

**验证**：此步完成后编译通过，但还没有可运行的输出。

### 第四步——PIT 驱动实现：配置 Channel 0、tick 计数、uptime 打印

**目标**：实现 PIT::init() 配置 Channel 0 为 100 Hz 方波生成器，实现 PIT::irq0_handler() 递增 tick 计数并每秒打印一次 uptime，以及 C-linkage 桥接函数 pit_irq0_handler()。

**代码**（文件路径：`kernel/drivers/pit.cpp`）：

```cpp
#include "kernel/drivers/pit.hpp"
#include <stdint.h>
#include "kernel/arch/x86_64/io.hpp"
#include "kernel/arch/x86_64/pic.hpp"
#include "kernel/lib/kprintf.hpp"

using cinux::arch::PIC;
using cinux::io::io_outb;
using cinux::lib::kprintf;

namespace cinux::drivers {

uint64_t PIT::tick_count_ = 0;
uint32_t PIT::freq_hz_    = 100;

void PIT::init(uint32_t freq_hz) {
    freq_hz_ = freq_hz;

    uint32_t divisor = PitHW::BASE_FREQ / freq_hz;
    if (divisor > 65535) divisor = 65535;
    if (divisor == 0) divisor = 1;

    // Command: channel 0, LSB-then-MSB, square wave, binary
    io_outb(PitHW::COMMAND, PitHW::CMD_CHANNEL_0 |
                             PitHW::CMD_LSB_MSB |
                             PitHW::CMD_MODE_3 |
                             PitHW::CMD_BINARY);

    // Divisor: low byte first, then high byte
    io_outb(PitHW::CHANNEL_0, static_cast<uint8_t>(divisor & 0xFF));
    io_outb(PitHW::CHANNEL_0, static_cast<uint8_t>((divisor >> 8) & 0xFF));

    tick_count_ = 0;
    kprintf("[PIT] Initialised at %u Hz (divisor=%u)\n", freq_hz_, divisor);
}
```

init() 函数做了三件事：计算除数、写命令字节、写除数值。

除数的计算是 `1193182 / freq_hz`，结果限制在 1 到 65535 之间（16 位除数的范围）。对于 100 Hz 的请求频率，除数是 11931，完全在范围内。`divisor == 0` 的检查是防御性的——如果有人传了一个比 1193182 还大的频率值，整数除法会得到 0，PIT 除数为 0 的行为是未定义的（有的资料说它会被解释为 65536，但不要依赖这种行为）。

命令字节 `0x36` 的位域拆解在概念精讲部分已经分析过了，这里就不再重复。关键是写入顺序——必须先写 Command Register（0x43），再写两次 Channel 0 Data Port（0x40）：第一次写低字节，第二次写高字节。PIT 内部有一个 8 位寄存器来追踪"下一个写入是低字节还是高字节"，这个状态由命令字节的 bit 5-4 决定——`CMD_LSB_MSB = 0x30` 告诉 PIT "接下来两次写入分别是 LSB 和 MSB"。

接下来是中断处理函数——这是本章最关键的部分之一：

```cpp
void PIT::irq0_handler(cinux::arch::InterruptFrame* /*frame*/) {
    tick_count_++;

    // Print uptime once per second (every freq_hz_ ticks)
    if ((tick_count_ % freq_hz_) == 0) {
        uint64_t seconds = tick_count_ / freq_hz_;
        kprintf("[TICK] uptime: %us\n", static_cast<unsigned>(seconds));
    }

    // Signal End-Of-Interrupt
    PIC::send_eoi(0);
}

uint64_t PIT::get_ticks() { return tick_count_; }

uint64_t PIT::get_uptime_ms() {
    return (tick_count_ * 1000) / freq_hz_;
}

uint32_t PIT::freq_hz() { return freq_hz_; }

}  // namespace cinux::drivers

// C-linkage bridge for assembly ISR stub
extern "C" void pit_irq0_handler(cinux::arch::InterruptFrame* frame) {
    cinux::drivers::PIT::irq0_handler(frame);
}
```

irq0_handler 的逻辑非常清晰：每次被调用，tick_count_ 加 1；当 tick_count_ 是 freq_hz_（100）的整数倍时，说明又过了一秒，打印 uptime。最后一行 `PIC::send_eoi(0)` 极其关键——这是告诉 Master PIC "IRQ0 已经处理完了，你可以接收下一个中断了"。如果漏掉这一行，PIT 中断只会来一次就再也不来了，因为 PIC 认为上一个 IRQ0 还没处理完。

注意 `%u` 格式化输出的 cast——`static_cast<unsigned>(seconds)`。我们的 `kprintf` 实现（上一章写的）不支持 `%lu` 或 `%llu`，所以需要把 `uint64_t` 截断为 `unsigned`（32 位）。这在 uptime 超过 2^32 秒（约 136 年）的时候会溢出，但说实话一个教学内核跑 136 年有点不现实。

文件最底部的 `extern "C"` 桥接函数是必要的，因为 `PIT::irq0_handler` 是一个 C++ 成员函数，经过 name mangling 之后链接器找不到它。ISR stub 在汇编里 `call pit_irq0_handler`，用的是 C 语言的命名规则，所以我们需要一个 `extern "C"` 的包装函数来做 C-linkage 到 C++ 的桥接。这个模式和上一章 exception handlers 里用的完全一样。

**验证**：此步完成后编译通过，但还没有可运行的输出。

### 第五步——IRQ 汇编 Stub：复用 ISR_NOERRCODE 宏生成 16 个跳板

**目标**：在 interrupts.S 里添加 16 个 IRQ stub（irq0_stub 到 irq15_stub），复用上一章写好的 `ISR_NOERRCODE` 宏。所有 IRQ 都没有硬件错误码，所以全部用 NOERRCODE 版本。

**代码**（文件路径：`kernel/arch/x86_64/interrupts.S`，追加内容）：

```asm
# ============================================================
# Hardware IRQ stubs (PIC remapped vectors 0x20-0x2F)
# ============================================================

/* Master PIC IRQs */
ISR_NOERRCODE irq0_stub,  pit_irq0_handler     /* IRQ0(0x20): PIT Timer */
ISR_NOERRCODE irq1_stub,  irq_default_handler   /* IRQ1(0x21): Keyboard */
ISR_NOERRCODE irq2_stub,  irq_default_handler   /* IRQ2(0x22): Cascade */
ISR_NOERRCODE irq3_stub,  irq_default_handler   /* IRQ3(0x23): COM2 */
ISR_NOERRCODE irq4_stub,  irq_default_handler   /* IRQ4(0x24): COM1 */
ISR_NOERRCODE irq5_stub,  irq_default_handler   /* IRQ5(0x25): LPT2 */
ISR_NOERRCODE irq6_stub,  irq_default_handler   /* IRQ6(0x26): Floppy */
ISR_NOERRCODE irq7_stub,  irq_default_handler   /* IRQ7(0x27): LPT1 */

/* Slave PIC IRQs */
ISR_NOERRCODE irq8_stub,  irq_default_handler   /* IRQ8(0x28):  RTC */
ISR_NOERRCODE irq9_stub,  irq_default_handler   /* IRQ9(0x29):  Free */
ISR_NOERRCODE irq10_stub, irq_default_handler   /* IRQ10(0x2A): Free */
ISR_NOERRCODE irq11_stub, irq_default_handler   /* IRQ11(0x2B): Free */
ISR_NOERRCODE irq12_stub, irq_default_handler   /* IRQ12(0x2C): PS/2 Mouse */
ISR_NOERRCODE irq13_stub, irq_default_handler   /* IRQ13(0x2D): FPU */
ISR_NOERRCODE irq14_stub, irq_default_handler   /* IRQ14(0x2E): Primary ATA */
ISR_NOERRCODE irq15_stub, irq_default_handler   /* IRQ15(0x2F): Secondary ATA */
```

这些 stub 和上一章的 CPU 异常 stub 完全同构——都使用 `ISR_NOERRCODE` 宏生成，都会保存/恢复所有通用寄存器，都会把 `InterruptFrame*` 通过 RDI 传递给 C 处理函数。唯一的区别是：IRQ0 的 handler 是 `pit_irq0_handler`（我们上一部步写的 PIT C-linkage 桥接函数），而 IRQ1-15 全部指向 `irq_default_handler`（一个什么都不做只发 EOI 的默认处理函数，定义在 irq_handlers.cpp 里）。

为什么 IRQ1-15 全用默认处理？因为我们目前只配置了 PIT 定时器这一个硬件中断源。键盘、磁盘、鼠标等设备我们还没写驱动，如果这些设备的中断来了（比如 QEMU 模拟的键盘可能会偶尔产生 IRQ1），我们需要有一个兜底的处理函数，至少把 EOI 发了，不让 PIC 卡住。`irq_default_handler` 就是这个兜底——它什么都不做，只是给 Master PIC 发一个 EOI，让 PIC 不要因为这个没人认领的中断而阻塞后续中断。

⚠️ 注意：`irq_default_handler` 里我们固定发的是 `PIC::send_eoi(0)`（Master EOI），而不是根据具体 IRQ 号来发。这在 Slave PIC 的中断到来时是不完全正确的（应该先给 Slave 发 EOI 再给 Master 发），但考虑到 Slave 上的设备（RTC、鼠标等）在 QEMU 默认配置下不会主动产生中断，这个问题暂时不会暴露。将来如果我们要支持 Slave PIC 上的设备，需要改进这个默认处理函数。

**验证**：此步完成后编译通过，但还没有可运行的输出。

### 第六步——IRQ 路由表：数据驱动的 IDT 注册

**目标**：在 irq_handlers.cpp 里实现 irq_init() 函数，用一个 constexpr 路由表把 16 个 IRQ stub 注册到 IDT 的 0x20-0x2F 向量位置。

**代码**（文件路径：`kernel/arch/x86_64/irq_handlers.cpp`）：

```cpp
#include <stdint.h>
#include "kernel/arch/x86_64/gdt.hpp"
#include "kernel/arch/x86_64/idt.hpp"
#include "kernel/arch/x86_64/pic.hpp"
#include "kernel/lib/kprintf.hpp"

using cinux::arch::PIC;
using cinux::arch::IDT;
using cinux::arch::g_idt;
using cinux::arch::make_idt_attr;

extern "C" {
void irq0_stub();
void irq1_stub();
// ... 所有 16 个 stub 声明 ...
void irq15_stub();
}

// Data-driven routing table
struct IRQRoute {
    uint8_t    vector;
    IDT::Stub  stub;
};

static constexpr IRQRoute k_irq_routes[] = {
    {0x20, irq0_stub},  {0x21, irq1_stub},  {0x22, irq2_stub},  {0x23, irq3_stub},
    {0x24, irq4_stub},  {0x25, irq5_stub},  {0x26, irq6_stub},  {0x27, irq7_stub},
    {0x28, irq8_stub},  {0x29, irq9_stub},  {0x2A, irq10_stub}, {0x2B, irq11_stub},
    {0x2C, irq12_stub}, {0x2D, irq13_stub}, {0x2E, irq14_stub}, {0x2F, irq15_stub},
};

static constexpr uint8_t kIRQAttr = make_idt_attr(
    IDTPrivilege::Kernel, IDTGateType::Interrupt);

// Default handler for IRQ1-15
extern "C" void irq_default_handler(InterruptFrame* /*frame*/) {
    PIC::send_eoi(0);
}

// Register all IRQ handlers
extern "C" void irq_init() {
    kprintf("[IRQ] Registering IRQ handlers (0x20-0x2F)...\n");

    for (const auto& route : k_irq_routes) {
        g_idt.set_handler(
            static_cast<ExceptionVector>(route.vector),
            route.stub, GDT_KERNEL_CODE, kIRQAttr, 0);
    }

    kprintf("[IRQ] All IRQ handlers registered.\n");
}
```

路由表的设计风格和上一章 IDT::init() 里的异常路由表一模一样——一个 `constexpr` 结构体数组，每条记录包含向量号和 stub 指针，然后用一个 range-for 循环统一注册。这种数据驱动的写法比手写 16 个 `set_handler` 调用要清晰得多——看一眼路由表就知道 IRQ0-15 被映射到了哪些 IDT 向量、每个向量用的是哪个 stub。

所有 IRQ 条目的属性都是一样的——`IDTPrivilege::Kernel`（DPL=0，只有内核态能触发）和 `IDTGateType::Interrupt`（Interrupt Gate，进入处理函数时自动关中断）。之所以用 Interrupt Gate 而不是 Trap Gate，是因为硬件中断处理期间我们不想被新的中断打断——如果 PIT 中断正在处理到一半，又来了一个新的 PIT 中断，tick_count_ 的递增可能会出问题（虽然对于简单的 `++` 操作来说这个问题不大，但养成好习惯总没错）。

**验证**：此步完成后编译通过，但还没有可运行的输出。

### 第七步——kernel_main 串起来：PIC init → IRQ init → PIT init → unmask → sti → halt loop

**目标**：在 kernel_main 里按正确顺序调用 PIC::init()、irq_init()、PIT::init()，然后 unmask IRQ0、执行 sti 开中断，最后进入hlt 空闲循环。

**代码**（文件路径：`kernel/main.cpp`）：

```cpp
#include <stdint.h>
#include "kernel/arch/x86_64/gdt.hpp"
#include "kernel/arch/x86_64/idt.hpp"
#include "kernel/arch/x86_64/pic.hpp"
#include "kernel/drivers/pit.hpp"
#include "kernel/lib/kprintf.hpp"

using cinux::arch::PIC;
using cinux::drivers::PIT;

extern "C" void irq_init();

extern "C" void kernel_main() {
    // 1. Serial port
    cinux::lib::kprintf_init();
    cinux::lib::kprintf("[BIG] Big kernel running @ 0x1000000\n");

    // 2. GDT (must come before IDT)
    cinux::arch::g_gdt.init();
    cinux::lib::kprintf("[BIG] GDT loaded.\n");

    // 3. IDT (depends on GDT selectors)
    cinux::arch::g_idt.init();
    cinux::lib::kprintf("[BIG] IDT loaded.\n");

    // 4. PIC: remap IRQ0-7 -> 0x20-0x27, IRQ8-15 -> 0x28-0x2F
    PIC::init();
    cinux::lib::kprintf("[BIG] PIC initialised.\n");

    // 5. IRQ handlers: register IRQ stubs in IDT
    irq_init();

    // 6. PIT: configure channel 0 at 100 Hz
    PIT::init(100);

    // 7. Test exception handling still works
    cinux::lib::kprintf("[BIG] Triggering int $3 breakpoint...\n");
    __asm__ volatile("int $3");
    cinux::lib::kprintf("[BIG] Breakpoint returned, continuing.\n");

    // 8. Unmask IRQ0 and enable interrupts
    PIC::unmask(0);
    cinux::lib::kprintf("[BIG] IRQ0 unmasked, enabling interrupts...\n");
    __asm__ volatile("sti");
    cinux::lib::kprintf("[BIG] Interrupts enabled. Entering idle loop.\n");

    // Idle loop: halt and wait for next interrupt
    while (1) {
        __asm__ volatile("hlt");
    }
}
```

初始化顺序是这一章的关键设计决策，我们来仔细梳理一下依赖链：

1. **Serial（kprintf_init）** 必须最先——后续所有 init 函数都会用 kprintf 打印状态。
2. **GDT** 必须在 IDT 之前——IDT 条目里的 selector 引用了 GDT 中的代码段。
3. **IDT** 必须在 PIC/IRQ 之前——IRQ 的 ISR stub 需要注册到 IDT 里，否则中断来了没地方跳。
4. **PIC** 必须在 irq_init 之前——虽然技术上 irq_init 只是往 IDT 里写条目，不直接操作 PIC，但逻辑上 PIC 重映射应该在 IRQ stub 注册之前完成（确保向量号对应关系正确）。
5. **irq_init** 在 PIC 之后、PIT 之前——这样 PIT 配置好之后，中断就真的能到达处理函数了。
6. **PIT** 在 irq_init 之后——PIT 配置完硬件就会开始产生中断信号，但此时 IRQ0 还被 mask 着，所以中断不会到达 CPU。
7. **int $3 测试** 在 sti 之前——验证 CPU 异常处理在 IRQ 基础设施就位后仍然正常工作。
8. **PIC::unmask(0) + sti** 最后执行——这是"点火"时刻：unmask(0) 打开 IRQ0 的 PIC 端门禁，sti 打开 CPU 端的 IF 标志，两者都打开后 PIT 的中断信号才能真正到达 CPU。

你可能注意到最后的 halt 循环从上一章的 `cli; hlt` 变成了单纯的 `hlt`。这是一个重要的变化——`cli; hlt` 会关掉中断然后暂停 CPU，但如果没有中断到来 CPU 就永远不会醒来。现在我们有了 PIT 定时器，用 `hlt` 就行了：CPU 暂停执行，PIT 中断到来时 CPU 醒来处理，处理完又回到 `hlt` 继续等待下一个中断。这样的 idle 循环比 `while(1) {}`（忙等待）好得多——hlt 状态下 CPU 几乎不耗电，而且不会占用总线带宽。

**验证**：这是我们可以第一次看到完整输出的步骤。构建并运行后，串口应该输出如下内容：

```
[BIG] Big kernel running @ 0x1000000
[BIG] GDT loaded.
[BIG] IDT loaded.
[BIG] PIC initialised.
[IRQ] Registering IRQ handlers (0x20-0x2F)...
[IRQ] All IRQ handlers registered.
[PIT] Initialised at 100 Hz (divisor=11931)
[BIG] Triggering int $3 breakpoint...

==== EXCEPTION: #BP (vector 3) ====
  RIP   = 0x...    CS  = 0x0008
  ...（寄存器 dump）...
========================================
[EXCEPTION] Breakpoint at RIP=0x...
[EXCEPTION] Continuing...
[BIG] Breakpoint returned, continuing.
[BIG] IRQ0 unmasked, enabling interrupts...
[BIG] Interrupts enabled. Entering idle loop.
[TICK] uptime: 1s
[TICK] uptime: 2s
[TICK] uptime: 3s
...
```

每秒稳定地出现一行 `[TICK] uptime: Ns`，直到你按 Ctrl+C 关闭 QEMU。

---

## 构建与运行

现在我们来构建并运行，看看内核的定时器中断是否真正工作。

```bash
# 从项目根目录
git checkout 011_big_kernel_pic_irq

# 配置 + 构建（Debug 模式）
cmake -B build -DCMAKE_BUILD_TYPE=Debug -S .
cmake --build build -j$(nproc)

# 运行
cd build
make run
```

QEMU 启动后，你应该会看到初始化消息逐行打印，然后 `int $3` 触发一次断点异常（打印寄存器 dump 后恢复），接着出现 `Interrupts enabled. Entering idle loop.`，之后就是每秒一行的 `[TICK] uptime: Ns`。如果看到这些输出，说明 PIC 重映射、PIT 配置、IRQ 处理、EOI 发送这一整条链路全部正确工作了。

QEMU 的启动参数在上一章已经解释过了。这里需要特别关注的是 `-serial stdio`——因为我们的 tick 信息是通过 `kprintf` 往串口写的，所以终端上能直接看到。如果发现 tick 信息来得太慢或者太快，很可能是 PIT 除数计算有误——可以用 QEMU 的 `-d int` 参数来追踪中断事件，确认 IRQ0 的频率是否符合预期。

---

## 调试技巧

### 中断只来一次就再也不来了

这是这一章最最常见的 bug，99% 的原因就是忘了在 IRQ handler 里调用 `PIC::send_eoi()`。8259A 在收到 EOI 之前不会转发下一个同优先级或更低优先级的中断——所以如果 PIT 的第一个 tick 来了、处理了、但没发 EOI，PIC 就会认为 IRQ0 还在处理中，后续所有 tick 都被阻塞。更隐蔽的情况是 EOI 发了但发错了——比如应该发 `PIC::send_eoi(0)`（IRQ0 对应 Master PIC），结果写成了 `PIC::send_eoi(1)`（发给 IRQ1 了，IRQ0 还是被阻塞）。

排查方法：在 `PIT::irq0_handler` 的开头和末尾各加一个 `kprintf`，看是进入了处理函数就不出来了，还是处理函数正常返回了但下一个 tick 不来了。如果只在开头看到了一次打印、末尾没看到，说明 `kprintf` 本身有问题或者 `send_eoi` 之前就 crash 了。如果两头都看到了但还是没有第二个 tick，检查 `send_eoi` 的参数是不是正确的 IRQ 号。

### sti 之后立即 Triple Fault

如果 `sti` 执行后 QEMU 直接重启（或因 `-no-reboot` 停在 Shutdown），说明有一个中断来了，但 CPU 查 IDT 找不到有效的处理程序，于是触发 #GP，#GP 的处理函数 halt 了（或者 #GP 的 IDT 条目也不存在，那就 Double Fault → Triple Fault）。

最可能的原因是 `irq_init()` 没有被调用，或者 `irq_init()` 注册的向量号和 PIC 重映射的目标不一致。比如 PIC 把 IRQ0 重映射到了 0x20，但 `irq_init()` 把 `irq0_stub` 注册到了 0x30（向量号写错了），那 IRQ0 来的时候 CPU 去 IDT[0x20] 查，发现是空的（Present=0），直接 #GP。

排查方法：在 `irq_init()` 里把注册的向量号打印出来，确认是 0x20-0x2F。也可以在 `PIC::init()` 里打印 `master_offset_` 和 `slave_offset_`，确认是 0x20 和 0x28。这两个值必须对上。

### PIT 除数写反了（高字节先写低字节后写）

PIT 要求先写低字节再写高字节——如果你写反了，PIT 会把第一个字节当作高字节、第二个字节当作低字节来拼装除数。结果就是实际除数和你预期的不一样，中断频率完全错误。比如你想设置除数 11931（0x2E9B），低字节 0x9B 先写、高字节 0x2E 后写；如果写反了，PIT 看到的除数就变成了 0x9B2E = 39726，对应频率约 30 Hz，不是我们想要的 100 Hz。

排查方法：用 GDB 在 `PIT::init` 的两次 `io_outb` 处打断点，确认写入顺序。或者在 `kprintf("[PIT] Initialised at %u Hz (divisor=%u)\n", ...)` 里打印除数值，确认是 11931。

---

## 本章小结

| 类别 | 名称 | 说明 |
|------|------|------|
| 类 | `cinux::arch::PIC` | 8259A PIC 驱动：init/send_eoi/mask/unmask/disable_all |
| 类 | `cinux::drivers::PIT` | Intel 8254 PIT 驱动：init/irq0_handler/get_ticks/get_uptime_ms |
| 命名空间 | `PicPort` | PIC I/O 端口常量（0x20/0x21/0xA0/0xA1）|
| 命名空间 | `PicICW` | PIC ICW 位域常量（INIT/ICW4/8086）|
| 命名空间 | `PitHW` | PIT 硬件常量（端口 0x40/0x43、BASE_FREQ 1193182）|
| 函数 | `PIC::init()` | ICW1-4 完整初始化 + io_wait + mask 恢复 |
| 函数 | `PIC::send_eoi()` | EOI 信号，Slave IRQ 需要 Master+Slave 双发 |
| 函数 | `PIT::init()` | 配置 Channel 0 方波模式，写入 16 位除数 |
| 函数 | `PIT::irq0_handler()` | 递增 tick，每秒打印 uptime，发 EOI |
| 函数 | `irq_init()` | 数据驱动路由表，注册 IRQ0-15 到 IDT 0x20-0x2F |
| 函数 | `irq_default_handler()` | IRQ1-15 兜底处理，只发 EOI |
| 汇编 | 16 个 IRQ stub | 复用 ISR_NOERRCODE 宏生成 |
| I/O 端口 | 0x20/0x21 | Master PIC Command/Data |
| I/O 端口 | 0xA0/0xA1 | Slave PIC Command/Data |
| I/O 端口 | 0x40/0x43 | PIT Channel 0 Data / Command Register |
| 指令 | `sti` / `cli` / `hlt` | 开中断 / 关中断 / CPU 暂停等待中断 |

本章我们从零搭建了大内核的硬件中断处理基础设施——PIC 重映射让 IRQ 脱离 CPU 异常向量区域，PIT 配置让系统有了稳定的时间源，IRQ 路由表把汇编 stub 注册到 IDT，irq0_handler 统计 tick 并每秒报告 uptime。从这一章开始，我们的内核不再只是一个"能响应异常"的程序，而是一个"能感知时间流逝"的系统。

下一章我们会在这个定时器的基础上继续扩展——引入键盘输入（IRQ1）、串口输入（IRQ3/4），或者进一步改进定时器为完整的调度器 tick。所有这些工作的基础都在这一章打好了——PIC 已经重映射，IDT 里的 IRQ 向量已经注册好，新的设备驱动只需要写对应的 handler 函数、在 IDT 里替换掉默认的 `irq_default_handler` 就行。
