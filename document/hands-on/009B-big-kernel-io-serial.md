# 009B Big Kernel I/O 端口与串口驱动 —— 让大内核"说话"

## 章节导语

上一章 (009A) 我们把大内核的启动基础设施搭建起来了——Bootloader 加载 mini kernel，mini kernel 从磁盘读取 ELF 格式的大内核、解析段、搬运到正确位置、然后跳转过去执行。但如果你仔细回忆一下，大内核的 `kernel_main()` 跑起来之后做的第一件事是什么？是调 `kprintf_init()` 然后打印一行 `[BIG] Big kernel running @ 0x1000000`。问题来了：这行字是怎么从 CPU 内部跑到你的终端屏幕上的？

答案就是这一章要讲的两层基础设施——x86 的 I/O 端口操作（`io.hpp`），以及建立在其上的 UART 16550 串口驱动（`serial.hpp` / `serial.cpp`）。前一章的大内核已经"活"了，但它是"沉默"的；这一章我们要给它一个声音，让它能够通过串口向外界输出文字。完成本章后，`make run` 时终端上出现的每一行内核输出，你都能理解它从 CPU 指令到 QEMU 终端的完整路径。

本章的前置知识是上一章（009A）的大内核启动流程。你需要知道大内核已经被加载到 0x1000000 并开始执行，但不了解具体的 I/O 操作细节完全没关系——这正是本章要覆盖的内容。

---

## 环境说明

我们仍然在 WSL2 (Linux 6.6.x) + QEMU 的环境中工作，工具链是 GCC/G++ 13+ 配合 CMake 构建。大内核使用 C++23 freestanding 编译（`-ffreestanding -fno-exceptions -fno-rtti`），没有标准库，所有底层操作全靠内联汇编直接和硬件对话。QEMU 默认将虚拟机的 COM1 串口映射到宿主机的 stdio，所以你在终端看到的输出就是内核写串口的结果。

---

## 第一步——x86 I/O 端口基础

### 什么是 I/O 端口？为什么不直接读写内存？

如果你之前写过嵌入式或者内核以外的程序，你习惯的硬件交互方式可能是读写某个内存地址——比如往 `0x12345678` 写一个值，某个外设就会做出反应。这种方式叫做 MMIO（Memory-Mapped I/O），确实是现代系统的主流方案，PCIe 设备、NVMe 硬盘、USB 控制器全都是 MMIO。

但 x86 架构从 8080 时代就保留了另一套独立的硬件通信机制——**I/O 端口**（Port I/O）。它和内存地址空间完全隔离，有自己的地址空间，有自己的 CPU 指令（`in` 和 `out`），甚至连地址宽度都不一样：I/O 端口地址空间只有 16 位宽，也就是说最多 65536 个端口（0x0000 到 0xFFFF），每个端口可以是 8 位、16 位或 32 位宽。这套机制看起来古老，但在 x86 上至今仍然存在，而且很多经典设备——包括我们马上要用的串口 UART——仍然挂在 I/O 端口上。

你可以把 I/O 端口理解为一排 65536 个"寄存器窗口"，每个窗口对应一个硬件寄存器或者控制端口。CPU 执行 `out 0x3F8, AL` 的时候，不是往内存地址 0x3F8 写数据，而是通过一条完全独立的总线告诉主板上的设备："端口 0x3F8 的寄存器，请接收这个字节"。反过来，`in AL, 0x3F8` 则是从端口 0x3F8 的寄存器读取一个字节到 AL。这套机制的好处是简单、确定性高——端口编号和功能之间的对应关系在硬件设计时就固定了，软件只需要知道"往哪个端口写什么值"就行。

关于 I/O 端口的权限问题也值得一提：`in` 和 `out` 指令是特权敏感的，当前特权级（CPL）必须小于等于 I/O 特权级（IOPL，存在 RFLAGS 寄存器里）才能执行。在 Ring 0（内核态）下这不是问题，因为 IOPL 通常被设为 0。但如果你的内核配置了用户态 I/O 权限（通过修改 EFLAGS.IOPL 或者设置 TSS 的 I/O Permission Bitmap），Ring 3 的程序也可以访问特定端口。我们的大内核运行在 Ring 0，所以不需要操心这个问题。

### io.hpp：内联汇编封装的 I/O 操作

现在我们来看大内核里的 I/O 端口操作是怎么实现的。整个文件只有一百多行，但每一行都值得仔细理解。

**代码**（文件路径：`kernel/arch/x86_64/io.hpp`）：

```cpp
#pragma once

#include <stdint.h>

namespace cinux::io {

// Byte I/O (8-bit)

inline uint8_t io_inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0"
                     : "=a"(value)
                     : "Nd"(port)
                     : "memory");
    return value;
}

inline void io_outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1"
                     :
                     : "a"(value), "Nd"(port)
                     : "memory");
}

// ... io_inw, io_outw, io_inl, io_outl 类似 ...
```

先看 `io_inb` 这个函数，它做的事情就是从指定的 I/O 端口读取一个字节。这里使用了 GCC 的扩展内联汇编语法，我们来逐字拆解这段 `__asm__ volatile` 语句。

汇编指令模板 `"inb %1, %0"` 就是 x86 的 `inb` 指令，AT&T 语法格式——源操作数在前，目标操作数在后。`%0` 和 `%1` 是占位符，分别对应输出操作数列表和输入操作数列表中的第一个和第二个操作数。

输出操作数 `"=a"(value)` 告诉编译器：`%0` 对应变量 `value`，约束字母 `a` 表示使用 `al`/`ax`/`eax`/`rax` 寄存器（根据变量大小自动选择，这里是 `uint8_t` 所以用 `al`），`=` 表示这是只写输出。

输入操作数 `"Nd"(port)` 告诉编译器：`%1` 对应变量 `port`，约束字母 `N` 表示这是一个 0-255 范围内的立即数常量，约束字母 `d` 表示可以使用 `dx` 寄存器。这两个约束是"或"的关系——编译器会根据 `port` 的值选择最合适的方式：如果 `port` 是编译期常量且在 0-255 范围内，就把它编码为指令中的立即数（`inb $imm, %al`）；如果不是常量或者超出范围，就用 `dx` 寄存器传递（`mov port, %dx; inb %dx, %al`）。这种双约束的设计让编译器有更大的优化空间。

clobber 列表 `"memory"` 的含义是：这条内联汇编可能会读写内存。这是一个编译器内存屏障，告诉编译器不要把这段代码前后的内存访问重排到这个位置之后/之前。对于 I/O 操作来说，这个约束非常重要——我们绝不希望编译器把一个"写端口 A"的操作重排到"读端口 B 的状态"之后，因为硬件的行为可能依赖于严格的操作顺序。

再来看 `io_outb`，它的结构和 `io_inb` 几乎一样，但有一个细微的区别：输出操作数列表是空的（`:`），只有输入操作数 `"a"(value), "Nd"(port)`。这是因为 `outb` 指令不产生返回值，它只是把 `al` 寄存器里的值写到端口里去。

你可能注意到了，mini kernel 里的 `io.h` 也有几乎一模一样的函数，但大内核版本多了一个重要的细节：所有函数都带 `"memory"` clobber。mini kernel 版本的 `io_inb` 和 `io_outb` 没有这个 clobber，在实际使用中大多数时候不会出问题，但从严谨性来说，I/O 操作应该被视为同步点——编译器不应该把内存访问跨过 I/O 操作重排，因为 I/O 操作的副作用可能和内存状态相关。大内核版本补上了这个遗漏，这是一个正确的做法。

关于 `volatile` 关键字也多说两句。`__asm__ volatile(...)` 里的 `volatile` 不是 C 语言里那个防止编译器缓存变量值的 `volatile`，而是专门针对内联汇编的修饰符——它告诉编译器"这段汇编代码必须保留，不要因为看起来没有输出就把它优化掉"。如果你把 `volatile` 去掉，编译器在进行死代码消除的时候可能会认为"这个 `outb` 没有产生任何可见的 C 语言层面的效果"然后把它删掉，这在 I/O 操作里是灾难性的——你发出去的硬件命令直接消失了。

### 16 位和 32 位的 I/O 操作

`io_inw`/`io_outw` 和 `io_inl`/`io_outl` 的结构与 8 位版本完全相同，只是把 `inb`/`outb` 换成了 `inw`/`outw` 和 `inl`/`outl`，变量类型也相应变成了 `uint16_t` 和 `uint32_t`。AT&T 语法里后缀 `b`/`w`/`l` 分别代表 byte（8 位）、word（16 位）、long（32 位），这个命名来自 x86 的历史传统。

```cpp
// Word I/O (16-bit)
inline uint16_t io_inw(uint16_t port) {
    uint16_t value;
    __asm__ volatile("inw %1, %0"
                     : "=a"(value)
                     : "Nd"(port)
                     : "memory");
    return value;
}

// Dword I/O (32-bit)
inline uint32_t io_inl(uint16_t port) {
    uint32_t value;
    __asm__ volatile("inl %1, %0"
                     : "=a"(value)
                     : "Nd"(port)
                     : "memory");
    return value;
}
```

不同宽度的 I/O 操作对应不同的硬件行为。很多设备的寄存器宽度是固定的——比如 ATA 磁盘控制器的数据端口是 16 位宽的，你必须用 `inw` 来读它；而串口 UART 的寄存器是 8 位宽的，用 `inb` 就够了。如果你用 `inl` 去读一个只有 8 位宽的端口，高 24 位的数据是未定义的——在某些硬件上可能返回 0，在另一些硬件上可能返回垃圾值。所以 I/O 操作的宽度必须和硬件寄存器宽度匹配。

### io_wait()：I/O 延迟的原理

```cpp
inline void io_wait() {
    io_outb(0x80, 0);
}
```

这个看起来很奇怪的函数值得专门解释一下。往端口 0x80 写一个 0，为什么能产生延迟？端口 0x80 是 x86 主板上的 POST 诊断端口（POST Diagnostics Port），在真实硬件上，对这个端口执行一次写操作大约需要 1 微秒。这个时间是怎么来的？因为 I/O 端口操作需要通过总线发送地址和数据，总线事务本身需要时间——在 ISA 总线时代，一次 I/O 端口操作大概需要 1us 左右。

1 微秒听起来很短，但对于某些硬件来说已经足够了。比如 PIT（可编程间隔定时器）和 PIC（可编程中断控制器）在某些操作之后需要短暂的恢复时间，连续发命令太快会导致硬件行为异常。`io_wait()` 就是为了满足这类时序要求而存在的。注意，在 QEMU 里这个延迟基本等于零——虚拟设备不需要恢复时间，但保留 `io_wait()` 是个好习惯，确保代码在真实硬件上也能正常工作。

现在我们把整个 `io.hpp` 梳理一下：它提供了 6 个 I/O 操作函数（inb/outb/inw/outw/inl/outl）加上 1 个延迟函数（io_wait），全部是 `inline` 的，使用内联汇编直接映射到 x86 的 `in`/`out` 指令。这些函数是所有硬件驱动的基础——磁盘驱动用它读写 ATA 寄存器，串口驱动用它操作 UART，以后的中断控制器、PCI 配置空间访问也全都要通过它们。把它们放在 `cinux::io` 命名空间里，既避免了全局命名冲突，又让调用代码的意图非常清晰。

---

## 第二步——UART 16550 硬件模型

### UART 是什么？

在写串口驱动之前，我们需要先搞清楚 UART 这个硬件到底在干什么。UART 的全称是 Universal Asynchronous Receiver-Transmitter（通用异步收发器），你可以把它理解为一个"串并转换器"——CPU 这边是并行数据（一个字节 8 位同时存在），而串口线那边是串行数据（一位一位地发送）。UART 负责在两者之间做转换：发送时把一个字节拆成 8 个位按顺序发出去，接收时把收到的 8 个位拼成一个字节交给 CPU。

x86 PC 上用的 UART 芯片通常是 16550A（或者它的兼容实现），它有 8 个寄存器，每个寄存器 8 位宽，映射到连续的 I/O 端口上。COM1 的基地址是 0x3F8，所以它的 8 个寄存器分别对应端口 0x3F8 到 0x3FF。

### 串口通信参数：8N1 是什么意思？

你会在串口驱动的注释里反复看到"115200 8N1"这个说法。拆开来看：

115200 是波特率（Baud Rate），意思是每秒传输 115200 个位（bit）。这个数字除以 10（8 个数据位 + 1 个起始位 + 1 个停止位）就是每秒大约能传 11520 个字节。波特率越高传输越快，但真实硬件上波特率太高会导致信号不稳定。

8N1 是对帧格式的缩写：8 个数据位（Data Bits）、无校验（No Parity）、1 个停止位（1 Stop Bit）。这是最常用的串口帧格式。起始位永远是 1 个（隐含的），所以一个完整的帧是：1 个起始位 + 8 个数据位 + 0 个校验位 + 1 个停止位 = 10 个位。校验位是可选的，可以设为奇校验（Odd Parity）或偶校验（Even Parity），用于检测传输错误，但在可靠的虚拟环境（QEMU）里不需要。

### COM 端口地址的来历

x86 PC 上有四个标准串口，地址是固定的：

```cpp
constexpr uint16_t SERIAL_COM1 = 0x03F8;
constexpr uint16_t SERIAL_COM2 = 0x02F8;
constexpr uint16_t SERIAL_COM3 = 0x03E8;
constexpr uint16_t SERIAL_COM4 = 0x2E8;
```

这四个地址不是随便定的，而是 IBM 在设计原始 PC 时就确定下来的，写在 BIOS 数据区（0000:0400 开始的 8 字节）里。QEMU 模拟的 PC 完全遵循这个布局，COM1 永远在 0x3F8。实际上绝大多数操作系统只使用 COM1（0x3F8）做内核控制台输出，COM2 偶尔用来接调制解调器或者红外端口，COM3 和 COM4 基本没人用。

### UART 寄存器偏移映射

UART 16550 有 8 个 8 位寄存器，但它们共享同一组 I/O 端口——通过不同的偏移和读/写方向来区分。这一点经常让新手迷惑，所以我们把映射关系列清楚：

```cpp
namespace SerialReg {
constexpr uint8_t RBR = 0;  // Receive Buffer Register  (read)
constexpr uint8_t THR = 0;  // Transmit Holding Register (write)
constexpr uint8_t IER = 1;  // Interrupt Enable Register
constexpr uint8_t FCR = 2;  // FIFO Control Register
constexpr uint8_t LCR = 3;  // Line Control Register
constexpr uint8_t MCR = 4;  // Modem Control Register
constexpr uint8_t LSR = 5;  // Line Status Register
constexpr uint8_t MSR = 6;  // Modem Status Register
constexpr uint8_t SCR = 7;  // Scratch Register
}
```

注意 RBR 和 THR 的偏移都是 0，但它们其实是两个不同的物理寄存器——读 offset 0 访问的是 RBR（接收缓冲），写 offset 0 访问的是 THR（发送保持）。这种"同一地址、不同方向对应不同寄存器"的设计在硬件里非常常见，它的好处是节省地址空间——毕竟 I/O 端口只有 65536 个，能省则省。

### 关键寄存器详解

在这 8 个寄存器中，我们真正需要关注的有五个。

**LCR（Line Control Register，偏移 3）** 控制串口帧格式。写入 0x03 表示 8 个数据位、无校验、1 个停止位，也就是 8N1。0x03 的二进制是 `00000011`，其中 bit 0-1 = 11 表示 8 位数据长度，bit 2 = 0 表示 1 个停止位，bit 3 = 0 表示无校验。LCR 还有一个特殊功能：bit 7 是 DLAB（Divisor Latch Access Bit），置 1 时 offset 0 和 offset 1 变成波特率除数寄存器的低字节和高字节。但我们不需要设置波特率（QEMU 虚拟 UART 的波特率无关紧要），所以 DLAB 保持为 0 就行。

**FCR（FIFO Control Register，偏移 2）** 控制 UART 内部的 FIFO 缓冲。写入 0xC7 的二进制是 `11000111`：bit 0 = 1 启用 FIFO，bit 1 = 1 清空接收 FIFO，bit 2 = 1 清空发送 FIFO，bit 6-7 = 11 设置 FIFO 触发阈值为 14 字节。16550 的 FIFO 缓冲区各有 16 字节深度，启用 FIFO 后 UART 可以在 CPU 来不及处理的时候暂存数据，避免丢失。

**MCR（Modem Control Register，偏移 4）** 控制调制解调器信号线。写入 0x03 表示置位 RTS（Request to Send，bit 1）和 DTR（Data Terminal Ready，bit 0）。在真实的串口通信中，RTS 和 DTR 是告诉对端"我准备好收发了"的握手信号。QEMU 的虚拟串口不关心这些信号，但设置它们不会出错，而且如果将来要在真实硬件上测试也能正常工作。

**LSR（Line Status Register，偏移 5）** 是只读的状态寄存器，驱动通过它来查询 UART 的当前状态。最重要的两个位是 bit 0（RX_READY，接收缓冲有数据）和 bit 5（TX_READY，发送保持寄存器为空）。我们在发送数据之前必须先检查 bit 5 是否为 1——如果 THR 还没空就往里写数据，上一个字节会被覆盖丢失。

**IER（Interrupt Enable Register，偏移 1）** 控制 UART 的中断使能。写入 0x00 表示禁用所有中断——我们的驱动使用纯轮询模式，不需要 UART 产生任何中断。如果你以后想做中断驱动的串口驱动（这样 CPU 不用在 putc 里空转等待），就需要在 IER 里启用相应的中断源，然后在 IDT 里注册一个中断处理函数。

---

## 第三步——Serial 驱动实现

现在我们有了 I/O 端口操作的基础，也理解了 UART 16550 的寄存器模型，接下来就是把这两者串起来，写一个能用的串口驱动。

### 头文件设计

**代码**（文件路径：`kernel/drivers/serial.hpp`）：

```cpp
namespace cinux::drivers {

class Serial {
public:
    explicit Serial(uint16_t port = SERIAL_COM1);
    void init(uint16_t port = SERIAL_COM1, uint32_t baud = 115200);
    void putc(char c);
    void puts(const char* s);
    bool is_ready() const;

private:
    uint16_t base_port_;
    bool is_tx_ready() const;
};

}
```

这里的设计有几个值得说的点。构造函数是 `explicit` 的，接受一个端口号参数，默认是 COM1（0x3F8）。构造函数只保存端口号，不做任何硬件配置——硬件初始化被放到了单独的 `init()` 方法里。这种"构造和初始化分离"的模式在内核开发里很常见，因为全局对象的构造发生在 C++ 运行时初始化阶段（`__ctors` 遍历），那时候中断可能还没设置好、内存管理器可能还没就绪，做硬件操作是不安全的。把初始化推迟到 `init()` 调用的时候，调用者就能控制初始化的时机。

`init()` 方法的参数列表看起来有点奇怪——它接受 `port` 和 `baud` 参数但标注了"unused"。这是因为大内核里的 `Serial` 类是从 mini kernel 的版本演进而来的，保留了接口的灵活性。当前实现只用构造函数传入的 `base_port_`，波特率在 QEMU 里不需要配置（虚拟 UART 的传输是即时的），但接口保留了将来在真实硬件上配置波特率的能力。

### 构造函数

```cpp
Serial::Serial(uint16_t port)
    : base_port_(port) {
    // Caller calls init() explicitly after construction.
}
```

构造函数只有一行：把端口号存到 `base_port_` 成员变量里。就这样，别的什么都不做。注释提醒调用者：构造之后记得调 `init()`。

### init()：配置 UART 寄存器

```cpp
void Serial::init(uint16_t /*port*/, uint32_t /*baud*/) {
    // Disable interrupts, set 8N1, enable FIFO, set MCR, verify LSR
    io_outb(base_port_ + SerialReg::IER, 0x00);
    io_outb(base_port_ + SerialReg::LCR, 0x03);
    io_outb(base_port_ + SerialReg::FCR, 0xC7);
    io_outb(base_port_ + SerialReg::MCR, 0x03);
    io_inb(base_port_ + SerialReg::LSR);
}
```

五条 I/O 操作，顺序很重要，我们来逐一理解。

第一步 `io_outb(base_port_ + IER, 0x00)` 禁用所有中断。在配置 UART 的过程中我们不希望它产生任何中断，否则如果中断控制器还没设置好，一个意外的硬件中断就会导致 triple fault。先禁中断是安全的做法。

第二步 `io_outb(base_port_ + LCR, 0x03)` 设置 8N1 帧格式。0x03 = 8 位数据 + 无校验 + 1 停止位，这是串口通信的"普通话"——绝大多数终端软件和虚拟终端都默认使用 8N1。同时这个值保证 DLAB 位（bit 7）为 0，不会误触波特率除数寄存器。

第三步 `io_outb(base_port_ + FCR, 0xC7)` 启用 FIFO 并清空缓冲。0xC7 = 启用 FIFO + 清空接收 FIFO + 清空发送 FIFO + 触发阈值 14 字节。在驱动刚启动的时候，UART 的 FIFO 里可能有残留的数据（尤其是如果 Bootloader 之前用过串口的话），清空它们确保我们从一个干净的状态开始。

第四步 `io_outb(base_port_ + MCR, 0x03)` 设置调制解调器控制信号。RTS + DTR 告诉对端（在这里就是 QEMU 的虚拟终端）"我准备好了"。在真实硬件上这些信号是必须的，QEMU 不检查但也不会出错。

第五步 `io_inb(base_port_ + LSR)` 是一次"探测性读取"——读一次 LSR 寄存器来验证 UART 确实存在且可访问。如果端口地址写错了（比如把 0x3F8 写成了 0x3F0），这次读取会返回 0xFF（浮空总线），虽然 init() 不检查这个返回值，但后续的 `is_tx_ready()` 检查会立刻发现问题——LSR 读到 0xFF 的话 bit 5 为 1，看起来 TX 一直是就绪的，所以至少不会卡死。不过你会在串口输出里看到乱码或者什么都没有，因为写到了错误的端口上。

对比一下 mini kernel 版本的 `init()` 会发现，大内核版本少了波特率除数寄存器的设置。在 mini kernel 里，init() 会先设 DLAB 位（LCR = 0x80），然后写除数低字节和高字节来配置波特率。但大内核省掉了这一步——QEMU 的虚拟 UART 不需要配置波特率，虚拟化层面数据传输是即时的，所谓的"115200 波特率"在虚拟环境里没有物理意义。保留这个简化不影响任何功能。

### putc()：轮询式字符发送

```cpp
void Serial::putc(char c) {
    while (!is_tx_ready()) {
        __asm__ volatile("pause");
    }
    io_outb(base_port_ + SerialReg::THR, static_cast<uint8_t>(c));
}
```

`putc()` 是整个串口驱动最核心的函数，它的工作方式叫做"轮询模式"（Polling Mode）。流程很简单：先检查 LSR 的 bit 5（TX_READY），如果为 0 就说明上一个字节还没发完，等；如果为 1 就说明 THR 空了，可以写新的字节了。

等待循环里的 `__asm__ volatile("pause")` 是一条 x86 hint 指令，它的作用是告诉 CPU "我现在在一个 spin-wait 循环里，你可以稍微降低功耗"。在不支持超线程的处理器上它基本是个 NOP，但在支持超线程的处理器上，`pause` 会让出执行资源给另一个逻辑线程，避免这个自旋循环独占整个物理核。这是一个好习惯——即使我们不关心功耗，一个会空转几百万次的循环也应该加上 `pause`。

和 mini kernel 版本相比，大内核的 `putc()` 少了超时计数器。mini kernel 的 putc 里有一个 `wait_count`，超过 100000 次循环就重置计数器（虽然重置之后什么也没做，相当于只是个占位）。大内核版本直接去掉了这个机制——在 QEMU 环境里，虚拟 UART 的发送是即时的，TX_READY 几乎不会等待；即使将来上真实硬件，如果 UART 真的坏到永远不就绪，内核也不应该静默跳过——卡死在这里反而更容易排查问题。

### puts()：字符串输出与换行转换

```cpp
void Serial::puts(const char* s) {
    if (s == nullptr) {
        return;
    }

    while (*s != '\0') {
        if (*s == '\n') {
            putc('\r');
        }
        putc(*s);
        s++;
    }
}
```

`puts()` 逐字符调用 `putc()`，但在遇到 `\n`（换行符，0x0A）的时候，会先发一个 `\r`（回车符，0x0D）。这个转换是串口终端的惯例——串口终端（以及大多数终端模拟器）使用 CR+LF（`\r\n`，即 0x0D 0x0A）作为换行标记，而 C 语言的字符串只用 `\n`（LF）表示换行。如果你不加 `\r`，终端会输出"阶梯状"的文字——每行开头和上一行开头不对齐，因为终端的光标只往下移了但没有回到行首。

这个坑我第一次写串口驱动的时候踩过，当时对着终端上一排排阶梯状的输出百思不得其解，还以为是什么编码问题。实际上就是少了 `\r` 而已。所以现在每次写串口 puts 我都会自动加上这个转换，已经形成肌肉记忆了。

空指针检查 `if (s == nullptr) return;` 是防御性编程——在内核里，一个空指针解引用就是 triple fault，而且串口驱动的 puts 通常用在 kprintf 里，如果某个格式化参数碰巧是 NULL（比如 `%s` 打印了一个空字符串指针），直接传给 puts 就会崩。返回空不输出总比内核崩溃好。

### is_ready() 和 is_tx_ready()

```cpp
bool Serial::is_tx_ready() const {
    return (io_inb(base_port_ + SerialReg::LSR) & SerialLSR::TX_READY) != 0;
}

bool Serial::is_ready() const {
    return is_tx_ready();
}
```

`is_tx_ready()` 是私有方法，读取 LSR 寄存器然后检查 bit 5（TX_READY = 0x20）。`is_ready()` 是公开接口，目前直接委托给 `is_tx_ready()`。之所以分两层，是因为将来可能会加上 RX 就绪的检查——比如 `is_ready()` 可能变成"TX 和 RX 都就绪"的语义，但 `is_tx_ready()` 永远只检查发送端。这种设计留了一点扩展余地。

### kprintf 如何使用 Serial

最后我们把视角拉远一点，看看 `Serial` 类是怎么被 `kprintf` 使用的。在 `kprintf.cpp` 里有一个文件级的全局 `Serial` 对象：

```cpp
static Serial g_serial(SERIAL_COM1);
```

这个全局对象在程序启动时被构造（C++ 全局对象的构造函数在 `__ctors` 段里被调用），但只是保存了 `base_port_ = 0x3F8`，不做硬件操作。真正的初始化发生在 `kernel_main()` 调用 `kprintf_init()` 的时候：

```cpp
void kprintf_init() {
    g_serial.init();
}
```

之后 `kprintf` 每次格式化一个字符，就通过 lambda 调用 `g_serial.putc()`：

```cpp
void kprintf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vkprintf_impl([&](char c) { g_serial.putc(c); }, fmt, args);
    va_end(args);
}
```

整个调用链条就是：`kprintf("Hello %s\n", name)` -> 格式化引擎逐个字符输出 -> lambda 调用 `g_serial.putc(c)` -> `putc` 轮询等待 TX_READY -> `io_outb(0x3F8, c)` -> QEMU 的虚拟 UART 收到字节 -> 显示在终端上。

---

## 构建与运行

```bash
# 从项目根目录
cmake -B build -DCMAKE_BUILD_TYPE=Debug -S .
cmake --build build -j$(nproc)
cd build && make run
```

**期望输出**：

```
[BIG] Big kernel running @ 0x1000000
```

这行输出看起来简单，但它经过了完整的调用链：`kernel_main()` -> `kprintf_init()` -> `g_serial.init()` 配置 UART -> `kprintf()` 格式化字符串 -> `g_serial.putc()` 轮询发送每个字节 -> `io_outb(0x3F8, byte)` 写 I/O 端口 -> QEMU 把字节显示到终端。

QEMU 的串口参数说明：启动参数 `-serial stdio` 将虚拟机的 COM1（I/O 端口 0x3F8 开始）连接到宿主机的标准输入/输出。当内核往端口 0x3F8 写一个字节时，QEMU 的虚拟 UART 收到这个字节，然后把它作为普通字符输出到你的终端。反过来，你在终端里敲键盘，QEMU 也会把字符通过端口 0x3F8 传给内核——只是我们的驱动目前还没实现 `getc()` 的上层调用，所以键盘输入暂时被忽略。

---

## 调试技巧

**串口没有任何输出**

这是最让人头疼的情况——内核跑起来了但终端一片空白。排查步骤是这样的：首先确认 `kprintf_init()` 确实被调用了，在 `kernel_main()` 里紧挨着 `kprintf_init()` 之后加一个直接端口写操作试试：`io_outb(0x3F8, 'A');`。如果这样能看到字符 'A'，说明 I/O 端口操作没问题，问题在 `Serial` 类的初始化或者 `kprintf` 的格式化逻辑。如果这样也看不到，问题可能在更底层——大内核根本没有开始执行，或者 QEMU 的串口映射配置不对。用 `make run-debug` 启动 GDB，在 `kernel_main` 设断点确认是否能命中。

**输出乱码或者阶梯状换行**

乱码通常是端口号写错了——比如把 0x3F8 写成了 0x3F0，写到了别的硬件设备上。阶梯状换行则是忘了在 `\n` 前面加 `\r`，检查 `puts()` 里的换行转换逻辑。

**putc() 卡死不返回**

这种情况说明 LSR 的 bit 5 永远不变为 1，通常是 `base_port_` 没有被正确设置。用 GDB 断在 `putc()` 里，检查 `this->base_port_` 的值是不是 0x3F8。如果是一个垃圾值（比如 0 或者 0xFFFFFFFF），说明 Serial 对象的构造有问题——可能是全局对象构造时机不对，或者内存被踩了。

**用 GDB 观察串口寄存器**

```bash
(gdb) break cinux::drivers::Serial::init
(gdb) continue
# 断在 init 入口
(gdb) print base_port_     # 应该是 0x3F8
(gdb) next                  # 执行第一条 IER 写操作
(gdb) step                  # 单步进入 io_outb
```

在 QEMU monitor（Ctrl+A, C 切换到 monitor）里可以直接检查虚拟 UART 的状态：`info qtree` 看 UART 设备是否被正确模拟，`xp /1bx 0x3f8+5` 看 LSR 寄存器的值（注意 QEMU monitor 的 xp 命令读的是虚拟地址映射后的 I/O 空间，不一定和端口 I/O 一致）。

---

## 本章小结

| 组件 | 关键函数/常量 | 说明 |
|------|-------------|------|
| I/O 端口操作 | `io_inb()`, `io_outb()`, `io_inw()`, `io_outw()`, `io_inl()`, `io_outl()` | x86 in/out 指令的内联汇编封装 |
| I/O 延迟 | `io_wait()` | 写端口 0x80 产生约 1us 延迟 |
| COM 端口地址 | `SERIAL_COM1 = 0x3F8` ~ `SERIAL_COM4 = 0x2E8` | x86 标准串口基地址 |
| UART 寄存器 | `SerialReg::THR/IER/FCR/LCR/MCR/LSR` | offset 0-7 的寄存器映射 |
| LSR 状态位 | `SerialLSR::TX_READY = 0x20` | bit 5 = THR 空，可以发送 |
| Serial 类 | `init()`, `putc()`, `puts()`, `is_ready()` | UART 16550 轮询驱动 |
| 换行转换 | `puts()` 中 `\n` -> `\r\n` | 串口终端需要 CR+LF |
| 自旋提示 | `__asm__ volatile("pause")` | 降低 spin-wait 功耗 |
| kprintf 集成 | `g_serial` (全局 Serial 对象) | kprintf_init() 初始化，kprintf() 调用 putc() |

这一章我们从最底层的 x86 I/O 端口指令讲起，理解了内联汇编的每一个约束细节，然后学习了 UART 16550 的硬件模型和寄存器布局，最后把它们组装成一个完整的串口驱动。从现在开始，大内核有了"说话"的能力——所有后续的调试输出、内核日志、panic 信息，都建立在这一章的基础设施之上。

---

## 下一章预告

下一章 (009C) 我们会在串口驱动的基础上构建 `kprintf`——一个内核专用的格式化打印函数，支持 `%d`、`%x`、`%p`、`%s` 等格式化占位符。kprintf 是内核开发中使用频率最高的工具函数之一，有了它我们就能在内核运行时打印变量值、内存地址、状态信息，整个调试体验会发生质的飞跃。
