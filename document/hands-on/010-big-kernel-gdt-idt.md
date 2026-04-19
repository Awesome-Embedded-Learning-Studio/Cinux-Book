# 010 Big Kernel GDT/IDT/中断系统 —— 触发异常，打印寄存器，然后活着说出来

## 章节导语

上一章（009）我们做了很多事情——把大内核链接到 higher-half 虚拟地址、写了串口驱动和 `kprintf`、修了 ELF 加载器的一个坑、还用 1GB 大内核做了一轮压力测试。到了这一章的起点，大内核已经能跑起来了，串口也能打字了，但说实话它还是个"聋哑人"——CPU 遇到除零、缺页、非法指令这些异常的时候，只会 triple fault 然后 QEMU 重启，连一行遗言都留不下来。开发内核最痛苦的事情莫过于此：你写了半天代码，跑起来直接黑屏重启，连哪里出的问题都不知道。

这一章我们要给大内核装上完整的异常处理基础设施：GDT（全局描述符表）、IDT（中断描述符表）、ISR stub（中断服务例程的汇编跳板）、以及一组 C++ 异常处理函数。完成本章后，我们会在 `kernel_main` 里用一条 `int $3` 指令触发断点异常，然后在串口看到完整的寄存器 dump——触发异常不死机，能看到 CPU 当时的完整状态，然后程序还能继续执行。这就是本章的目标。

本章的前置知识是上一章（009_large_kernel_entry）的大内核启动流程和 `kprintf` 串口输出。如果你还没读完 009 系列的 A~E 五个子章节，建议先回去补完，尤其是 009A（boot.S 启动汇编）和 009B/009C（串口驱动和 kprintf），因为本章的异常处理函数依赖 `kprintf` 来输出寄存器信息。

---

## 概念精讲

### 大内核的 GDT：为什么不直接复用 Bootloader 的？

如果你跟着这个项目从头走过来的，你一定记得 002 章在 Bootloader 里配过一次 GDT，007 章在 mini kernel 里也配过一次。现在到了大内核，我们又要配第三次——这感觉有点荒谬，但实际上是必要的。Bootloader 的 GDT 在实模式切换保护模式时起作用，mini kernel 的 GDT 放在低地址区域，而现在大内核运行在 higher-half 虚拟地址空间 `0xFFFFFFFF80000000`，我们需要在自己管理的内存里重新建立 GDT，确保段寄存器和 IDT 中引用的段选择子都指向正确的地方。

在 x86_64 的 long mode 下，分段机制已经被大幅弱化——基地址和限长基本被硬件忽略了（Base 被当作 0，Limit 被当作 `0xFFFFFFFFF`），但 CS/DS/SS 这些段寄存器仍然需要指向有效的 GDT 条目，CPU 才能正常工作。尤其是中断处理这个环节——IDT 里的每个条目都有一个"代码段选择子"字段，它指向 GDT 中的 code segment 描述符，CPU 在跳转到中断处理程序时会用这个选择子加载 CS 寄存器。如果 GDT 里没有正确的 code64 描述符，中断一触发就 triple fault。

这次我们的 GDT 比 mini kernel 的那次要"豪华"得多——不仅有内核态的 code 和 data 段，还预留了用户态的 code64 和 data64 段（为将来跑用户进程做准备），外加一个 TSS（Task State Segment）占位符。TSS 在 long mode 下的主要用途不是任务切换（那是 32 位时代的遗产），而是提供 IST（Interrupt Stack Table）和权限切换时的 RSP0 栈指针——当 CPU 从 Ring 3 切换到 Ring 0 处理中断时，它会自动从 TSS 里读取新的栈指针。

```
大内核的 GDT 布局：

索引  选择子   内容               用途
────  ──────   ────               ────
0     0x00     Null Descriptor    Intel 规定必须为空
1     0x08     Kernel Code64      内核代码段 (Ring 0, 64-bit)
2     0x10     Kernel Data64      内核数据段 (Ring 0, 32-bit legacy compat)
3     0x1B     User Code64        用户代码段 (Ring 3, 64-bit, RPL=3 → 0x18|3=0x1B)
4     0x23     User Data64        用户数据段 (Ring 3, 32-bit, RPL=3 → 0x20|3=0x23)
5-6   0x28     TSS (16 bytes)     任务状态段占位 (2 个 GDT slot)
```

选择子的值计算方式是 `索引 × 8 + RPL`，其中 RPL（Requested Privilege Level）对于内核态是 0，对于用户态是 3。所以 User Code64 的选择子是 `3 × 8 + 3 = 0x1B`，User Data64 是 `4 × 8 + 3 = 0x23`。

### IDT：CPU 异常的"电话簿"

IDT（Interrupt Descriptor Table）是 x86 架构的中断分发核心数据结构。CPU 遇到异常（比如除零、缺页、非法指令）或者收到外部硬件中断（比如键盘、定时器）时，会拿着一个 0-255 的向量号去 IDT 里查对应的处理程序地址，然后跳过去执行。如果没有 IDT，或者 IDT 里对应向量的条目是空的，CPU 就会触发 Double Fault（#DF），如果 Double Fault 也没人处理，就 Triple Fault，QEMU 直接重启——就是我们之前看到的那种"黑屏重启"。

IDT 在 64 位模式下每个条目占 16 字节（比 32 位模式多了 8 字节，因为地址从 32 位扩展到了 64 位），最多 256 个条目。前 32 个向量号（0-31）被 Intel 保留给 CPU 异常，比如向量 0 是除零（#DE）、向量 3 是断点（#BP）、向量 6 是非法指令（#UD）、向量 13 是一般保护错误（#GP）、向量 14 是页错误（#PF）。我们这一章只处理前 15 个（0-14），硬件中断的 IRQ 处理留到后面的章节。

```
IDT 条目的 16 字节布局：

Byte 0-1:  Offset[0:15]    处理程序地址的低 16 位
Byte 2-3:  Segment Selector 代码段选择子（指向 GDT 中的 code segment）
Byte 4:    IST              Interrupt Stack Table 偏移（0 = 不切换栈）
Byte 5:    Type/Attributes  位 7: Present, 位 5-6: DPL, 位 0-3: Gate Type
Byte 6-7:  Offset[16:31]   处理程序地址的中间 16 位
Byte 8-11: Offset[32:63]   处理程序地址的高 32 位
Byte 12-15: Reserved        保留（必须为 0）
```

### Interrupt Gate vs Trap Gate：一个关中断的区别

IDT 里有两种 64 位门类型：Interrupt Gate（类型 0xE）和 Trap Gate（类型 0xF）。它们的区别说起来极其简单——Interrupt Gate 在跳转到处理程序时 CPU 自动清除 RFLAGS.IF 标志（关中断），处理完后 IRETQ 恢复原来的 IF 状态；Trap Gate 不动 IF，中断保持原来的开关状态。

这个区别的设计意图是：对于致命异常（#DE、#GP、#PF 等），处理过程中不应该被新的中断打断，所以用 Interrupt Gate；对于调试相关的异常（#BP 断点、#DB 调试），它们本身就是在调试流程中触发的，关掉中断反而会让调试器无法正常工作，所以用 Trap Gate。我们的策略是：#BP（向量 3）和 #DB（向量 1）用 Trap Gate，其他所有异常用 Interrupt Gate。

还有一个设计决策是关于 DPL（Descriptor Privilege Level）的——#BP 和 #DB 的 DPL 设为 3（用户态可触发），其他异常的 DPL 设为 0（只有内核态能触发）。这样设计是因为 `int $3` 是调试器常用的软件断点指令，用户态程序也可能触发它（比如 GDB 的断点实现），所以必须允许 Ring 3 的代码通过这个向量进入内核。而其他异常要么是硬件自动触发的（不需要考虑 DPL），要么是内核不应该让用户态随便 `int` 的。

### 中断栈帧：CPU 和 ISR stub 的接力

当 CPU 响应异常时，它会自动把当前的 SS、RSP、RFLAGS、CS、RIP 压入栈中（这是硬件自动完成的，不可跳过）。对于某些异常（#DF、#TS、#NP、#SS、#GP、#PF），CPU 还会额外压入一个错误码（Error Code），包含关于异常原因的额外信息。对于没有硬件错误码的异常（比如 #DE、#BP），栈上就没有这个东西。

问题是：我们的 C 处理函数需要接收一个统一的 `InterruptFrame*` 参数来读取所有寄存器的值。如果有的异常栈上有错误码、有的没有，结构体就对不齐了。解决方案是在 ISR stub 里做一个"填零"操作——对于没有硬件错误码的异常，stub 自己 `push $0` 填一个假的错误码 0，这样所有异常到达 C 处理函数时，栈帧的布局就完全统一了。

```
中断栈帧 InterruptFrame 布局（从高地址到低地址）：

高地址
┌──────────────────┐
│  SS              │  ← CPU 自动压入
│  RSP             │
│  RFLAGS          │
│  CS              │
│  RIP             │
│  Error Code      │  ← CPU 压入（某些异常）或 stub push $0（其他异常）
├──────────────────┤  ← ISR stub 从这里开始保存
│  RAX             │
│  RBX             │
│  RCX             │
│  RDX             │
│  RBP             │
│  RSI             │
│  RDI             │
│  R8              │
│  R9              │
│  R10             │
│  R11             │
│  R12             │
│  R13             │
│  R14             │
│  R15             │
└──────────────────┘  ← RSP 指向这里，也就是 InterruptFrame* 的值
低地址
```

这个 `InterruptFrame` 结构体定义在 `idt.hpp` 里，字段顺序必须和汇编里 push 的顺序完全一致（R15 先 push、RAX 最后 push，所以结构体从上到下是 R15 到 RAX，然后是 error_code、RIP、CS、RFLAGS、RSP、SS），否则 C 代码读到的寄存器值全部是错的。这个坑说实话很难排查，因为编译器不会帮你检查结构体和汇编的对应关系——如果你的 RDI 和 RBP 字段写反了，程序不会崩，只是打印出来的值是错的，你可能会怀疑是 CPU 的问题而不是代码的问题。

### 致命异常 vs 非致命异常：halt 还是继续？

x86 的 CPU 异常大致可以分成两类：一类是"通知型"的（#BP 断点、#DB 调试），这类异常处理完后程序可以继续执行；另一类是"致命型"的（#DE 除零、#UD 非法指令、#GP 保护错误、#PF 页错误），这类异常意味着程序状态已经不可恢复了——除零的结果没处放、非法指令不知道该跳到哪里、页错误意味着内存访问完全错了。对于致命异常，我们打印完寄存器 dump 之后进入 `cli; hlt` 死循环，因为继续执行没有任何意义，只会引发更多的异常。

#PF（页错误）的处理稍微特殊一点——除了打印寄存器 dump，我们还会从 CR2 寄存器读出触发缺页的虚拟地址，然后把错误码的各个 bit 拆解成可读的字符串（是读还是写、是用户态还是内核态、是页不存在还是权限违反）。这些信息对于后续调试内存管理相关的 bug 非常有价值。

### AT&T 汇编语法速查

我们的 ISR stub 用 GNU AS（AT&T 语法）编写，这是整个项目统一使用的汇编语法。如果你之前一直跟着教程走，对 AT&T 语法应该已经比较熟悉了，但这里还是做一个针对性的速查，因为我们本章会大量使用以下指令：

| 操作 | AT&T 语法 | 含义 |
|------|-----------|------|
| 压栈 | `pushq %rax` | 把 RAX 的值压入栈 |
| 压立即数 | `pushq $0` | 把数字 0 压入栈 |
| 弹栈 | `popq %rax` | 从栈顶弹出到 RAX |
| 栈指针传参 | `movq %rsp, %rdi` | 把栈指针作为第一个参数 |
| 跳栈顶 | `addq $8, %rsp` | 跳过栈顶 8 字节（不读值）|
| 中断返回 | `iretq` | 从中断返回（恢复 RIP/CS/RFLAGS/RSP/SS）|
| 加载 GDT | `lgdt (%rsp)` | 从内存加载 GDTR |
| 加载 IDT | `lidt (%rsp)` | 从内存加载 IDTR |
| 远调用返回 | `lretq` | 远返回（用于刷新 CS）|
| 加载 TR | `ltr %ax` | 加载任务寄存器 |

操作数顺序永远是 `source, destination`——`movq %rsp, %rdi` 是把 RSP 的值复制到 RDI，不是反过来。寄存器前缀 `%`，立即数前缀 `$`，这两个符号漏写了汇编器会报错，而且报错信息有时候不太直观，容易让人以为是指令本身写错了。

---

## 动手实现

### 第一步——GDT 头文件：scoped enum 和 constexpr 工厂

**目标**：创建 GDT 的 C++ 头文件，用 class 封装 GDT 状态，用 scoped enum 表达段描述符的 access byte 和 flags byte，用 constexpr 工厂函数生成各种描述符。

我们这一章的 GDT 设计和 mini kernel 那次（007 章）有几个重要区别。首先是代码组织方式——不再用全局函数 + 全局数组，而是把 GDT 的状态（描述符数组、GDTR 指针、TSS 结构体）全部封装在一个 `GDT` class 里。其次是 API 类型安全——段描述符的 access byte 是一个 8 位的位域，不同 bit 代表不同含义，如果直接用 `uint8_t` 的话，调用者完全可以传入一个语义上毫无意义的值（比如 `access = 0xFF`），编译器不会报错，运行时 triple fault。用 scoped enum（`enum class SegmentAccess : uint8_t`）配合 `operator|` 重载，调用者只能用预定义的标志来组合，类型安全得到保证。

**代码**（文件路径：`kernel/arch/x86_64/gdt.hpp`）：

```cpp
#include <stdint.h>

namespace cinux::arch {

// Segment Selector Constants
constexpr uint16_t GDT_KERNEL_CODE = 0x08;
constexpr uint16_t GDT_KERNEL_DATA = 0x10;
constexpr uint16_t GDT_USER_CODE   = 0x1B;
constexpr uint16_t GDT_USER_DATA   = 0x23;
constexpr uint16_t GDT_TSS         = 0x28;

enum class SegmentAccess : uint8_t {
    Present    = 1u << 7,
    Ring0      = 0u << 5,
    Ring3      = 3u << 5,
    CodeData   = 1u << 4,
    Executable = 1u << 3,
    ReadWrite  = 1u << 1,
    TSS64Avail = 0x09,
};

constexpr SegmentAccess operator|(SegmentAccess a, SegmentAccess b) {
    return static_cast<SegmentAccess>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

enum class SegmentFlags : uint8_t {
    Granularity4K = 1u << 3,
    LongMode      = 1u << 1,
    Size32        = 1u << 2,
};
```

我们来看这些枚举值的含义。`SegmentAccess` 的每个成员对应 access byte 中的一个或几个 bit：`Present`（bit 7）表示这个段描述符有效、`CodeData`（bit 4）区分代码/数据段和系统段、`Executable`（bit 3）表示段可执行、`ReadWrite`（bit 1）表示段可读写。`Ring0` 和 `Ring3` 分别是 DPL=0 和 DPL=3。注意 `TSS64Avail = 0x09` 是一个完整的值（bit 3 置 1 表示 64 位 TSS 可用，bit 0 置 1 表示 Available），不是按 bit 组合的。

`SegmentFlags` 对应描述符第 6 字节的高 4 位：`Granularity4K` 表示 limit 的粒度是 4KB（而不是 1 字节），`LongMode` 表示这是一个 64 位代码段，`Size32` 表示默认操作数是 32 位。这里有个容易混淆的地方——内核数据段虽然也是 64 位模式下的，但它的 flags 里用的是 `Size32` 而不是 `LongMode`。原因是 64 位模式下只有代码段才需要设置 L（Long）位，数据段的 L 位被保留（必须为 0），数据段的 D/B 位设置成 1 即可。

接下来是 `GDT` class 本体和工厂函数：

```cpp
class GDT {
public:
    void init();

private:
    struct [[gnu::packed]] Entry {
        uint16_t limit_low;
        uint16_t base_low;
        uint8_t  base_middle;
        uint8_t  access;
        uint8_t  flags_limit_high;
        uint8_t  base_high;
    };
    static_assert(sizeof(Entry) == 8, "GDT entry must be 8 bytes");

    struct [[gnu::packed]] Pointer {
        uint16_t limit;
        uint64_t base;
    };

    struct [[gnu::packed]] TaskStateSegment {
        uint32_t reserved0;
        uint64_t rsp[3];
        uint64_t reserved1;
        uint64_t ist[7];
        uint64_t reserved2;
        uint16_t reserved3;
        uint16_t iomap_base;
    };
    static_assert(sizeof(TaskStateSegment) == 104, "TSS must be 104 bytes");

    // Constexpr factory functions
    static constexpr Entry null_entry() { return {0, 0, 0, 0, 0, 0}; }

    static constexpr Entry segment_entry(SegmentAccess access, SegmentFlags flags) {
        return {
            .limit_low        = 0xFFFF,
            .base_low         = 0,
            .base_middle      = 0,
            .access           = static_cast<uint8_t>(access),
            .flags_limit_high = static_cast<uint8_t>((static_cast<uint8_t>(flags) << 4) | 0x0F),
            .base_high        = 0,
        };
    }

    static constexpr Entry tss_low_entry(uint64_t base, uint32_t limit);
    static constexpr Entry tss_high_entry(uint64_t base);

    static constexpr auto kEntryCount = 7;
    Entry entries_[kEntryCount]{};
    Pointer gdtr_{};
    TaskStateSegment tss_{};

    void load();
};

extern GDT g_gdt;
```

工厂函数 `segment_entry` 接受两个 scoped enum 参数（access 和 flags），把它们组装成一个 8 字节的 GDT 描述符。其中 `flags_limit_high` 字段的高 4 位放 flags（左移 4 位），低 4 位放 limit 的高 4 位（固定为 0x0F，配合 `limit_low = 0xFFFF`，总 limit 为 `0xFFFFF`，再配合 `Granularity4K` 就是 4GB 的完整地址空间）。Base 全部填 0——在 long mode 下硬件会忽略 base 和 limit，但填 0 是最安全的做法。

TSS 描述符比较特殊——它是系统段描述符，占 16 字节（两个 GDT slot），因为 64 位模式下 TSS 的基地址是 64 位的，需要额外的 8 字节来存放高 32 位地址。`tss_low_entry` 生成前 8 字节（包含 limit、base 的低 32 位、access byte），`tss_high_entry` 生成后 8 字节（包含 base 的高 32 位）。我们的 TSS 目前只是一个占位符——所有字段都是 0，在设置好 IST 或用户态切换之前它不会起实际作用，但 GDT 里必须有这个条目，否则 `ltr` 指令会触发 #GP。

`static_assert(sizeof(Entry) == 8)` 和 `static_assert(sizeof(TaskStateSegment) == 104)` 这两个编译期断言非常重要。GDT 条目必须是精确的 8 字节，TSS 在 64 位模式下必须是精确的 104 字节（Intel SDM Vol. 3A Table 8-2 规定的）。如果结构体大小不对——比如忘了 `[[gnu::packed]]` 导致编译器插了 padding——这些断言会在编译期就报错，而不是运行时 triple fault 让你一脸懵。

**验证**：此步完成后编译应该通过，但还没有可运行的输出。

### 第二步——GDT 初始化：填充描述符、lgdt、刷新段寄存器

**目标**：实现 `GDT::init()` 填充所有 7 个 GDT 条目（null、kernel code、kernel data、user code、user data、TSS low、TSS high），然后实现 `GDT::load()` 通过 `lgdt` 指令加载 GDTR，用远返回（lretq）刷新 CS，手动设置 DS/ES/FS/GS/SS，最后 `ltr` 加载任务寄存器。

**代码**（文件路径：`kernel/arch/x86_64/gdt.cpp`）：

```cpp
#include "kernel/arch/x86_64/gdt.hpp"
#include <stdint.h>

namespace cinux::arch {

GDT g_gdt;

void GDT::init() {
    entries_[0] = null_entry();

    // Kernel Code64: Present | Ring0 | CodeData | Executable | ReadWrite
    entries_[1] = segment_entry(
        SegmentAccess::Present | SegmentAccess::Ring0 |
        SegmentAccess::CodeData | SegmentAccess::Executable | SegmentAccess::ReadWrite,
        SegmentFlags::Granularity4K | SegmentFlags::LongMode);

    // Kernel Data: Present | Ring0 | CodeData | ReadWrite
    // Note: data segments use Size32 (not LongMode) in long mode
    entries_[2] = segment_entry(
        SegmentAccess::Present | SegmentAccess::Ring0 |
        SegmentAccess::CodeData | SegmentAccess::ReadWrite,
        SegmentFlags::Granularity4K | SegmentFlags::Size32);

    // User Code64: Present | Ring3 | CodeData | Executable | ReadWrite
    entries_[3] = segment_entry(
        SegmentAccess::Present | SegmentAccess::Ring3 |
        SegmentAccess::CodeData | SegmentAccess::Executable | SegmentAccess::ReadWrite,
        SegmentFlags::Granularity4K | SegmentFlags::LongMode);

    // User Data: Present | Ring3 | CodeData | ReadWrite
    entries_[4] = segment_entry(
        SegmentAccess::Present | SegmentAccess::Ring3 |
        SegmentAccess::CodeData | SegmentAccess::ReadWrite,
        SegmentFlags::Granularity4K | SegmentFlags::Size32);

    // TSS (2 entries: low 8 bytes + high 8 bytes)
    const auto tss_addr = reinterpret_cast<uint64_t>(&tss_);
    entries_[5] = tss_low_entry(tss_addr, sizeof(TaskStateSegment) - 1);
    entries_[6] = tss_high_entry(tss_addr);

    gdtr_.limit = sizeof(entries_) - 1;
    gdtr_.base  = reinterpret_cast<uint64_t>(entries_);

    load();
}
```

`init()` 函数的逻辑很直接：按索引顺序填充 7 个 GDT 条目，然后设置 GDTR 的 limit（GDT 总字节数减 1）和 base（GDT 数组的起始地址），最后调用 `load()` 把 GDTR 加载到 CPU。

你可能注意到 `g_gdt` 是一个全局变量，定义在 `.bss` 段里（因为 `GDT` class 的所有成员都有默认初始化值）。boot.S 在调用 `kernel_main` 之前会清零 BSS 段，所以 `g_gdt` 的所有字段在 `init()` 被调用之前都是零。这正是我们想要的——零初始化的 GDT 条目就是无效的（Present 位为 0），不会被 CPU 误用。

接下来是 `load()` 函数，这里面有 inline assembly：

```cpp
void GDT::load() {
    __asm__ volatile(
        "lgdt %[gdtr]\n\t"
        "pushq %[cs]\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        "movw %[ds], %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        "movw %%ax, %%ss\n\t"
        :
        : [gdtr] "m"(gdtr_), [cs] "i"(GDT_KERNEL_CODE), [ds] "i"(GDT_KERNEL_DATA)
        : "rax", "memory");

    const uint16_t tss_sel = GDT_TSS;
    __asm__ volatile("ltr %[sel]\n\t" : : [sel] "r"(tss_sel) : "memory");
}
```

这段汇编做了两件事：加载 GDTR 和刷新所有段寄存器。

`lgdt %[gdtr]` 把 GDTR 指针指向的 10 字节数据（2 字节 limit + 8 字节 base）加载到 CPU 的 GDTR 寄存器。这一步本身很简单，但它只修改了 GDTR，不影响已经加载到 CS 里的旧选择子。所以我们需要手动刷新 CS——做法是在栈上伪造一个远返回地址（先 push 新的 CS 值，再 push 下一条指令的地址），然后用 `lretq` 弹出这两个值到 CS:RIP，实现"跳转到同一个地方但换了 CS"的效果。这里的 `1f` 是 GCC 的局部标签语法，表示"向前找第一个名为 `1` 的标签"，`%%rip` 是位置无关的地址计算（因为我们的内核链接在 higher-half，绝对地址在运行时可能因为加载位置不同而无效，用 RIP-relative 更安全）。

刷新完 CS 之后，还要把 DS/ES/FS/GS/SS 全部设置为 kernel data selector（0x10）。这里先把选择子值放到 AX，然后用 AX 加载各个段寄存器——不能直接用立即数加载段寄存器，x86 不支持 `mov $0x10, %ds` 这样的指令。

最后是 `ltr %[sel]`，把 TSS 选择子（0x28）加载到任务寄存器（TR）。这一步必须在 GDT 加载之后执行，因为 `ltr` 会去 GDT 里查 0x28 对应的描述符，验证它是有效的 TSS 描述符。如果 GDT 还没加载或者 0x28 处的描述符无效，`ltr` 会触发 #GP。

`__asm__ volatile` 的 `volatile` 关键字告诉编译器不要优化掉这段汇编（因为它的副作用是修改 CPU 的内部寄存器，而不是修改内存），`"memory"` clobber 告诉编译器这段汇编可能修改了内存（实际上是修改了段寄存器相关的隐式状态），防止编译器把前后的内存操作重排到汇编块的错误一侧。

**验证**：此步完成后，GDT 的 init 和 load 可以编译通过，但还不能独立运行验证。

### 第三步——IDT 头文件：InterruptFrame、scoped enum、class 封装

**目标**：创建 IDT 的 C++ 头文件，定义 `InterruptFrame` 结构体（和汇编 ISR stub 的栈帧布局精确匹配），用 scoped enum 表达异常向量号、门类型和特权级，用 class 封装 IDT 状态。

**代码**（文件路径：`kernel/arch/x86_64/idt.hpp`）：

```cpp
#pragma once
#include <stdint.h>

namespace cinux::arch {

enum class ExceptionVector : uint8_t {
    DE  = 0,    // #DE: Divide Error
    DB  = 1,    // #DB: Debug Exception
    NMI = 2,    // Non-maskable Interrupt
    BP  = 3,    // #BP: Breakpoint (INT3)
    OF  = 4,    // #OF: Overflow
    BR  = 5,    // #BR: BOUND Range Exceeded
    UD  = 6,    // #UD: Invalid Opcode
    NM  = 7,    // #NM: Device Not Available
    DF  = 8,    // #DF: Double Fault (has error code)
    TS  = 10,   // #TS: Invalid TSS (has error code)
    NP  = 11,   // #NP: Segment Not Present (has error code)
    SS  = 12,   // #SS: Stack-Segment Fault (has error code)
    GP  = 13,   // #GP: General Protection (has error code)
    PF  = 14,   // #PF: Page Fault (has error code)
};

enum class IDTGateType : uint8_t {
    Interrupt = 0x0E,  // 64-bit interrupt gate (clears IF)
    Trap      = 0x0F,  // 64-bit trap gate (preserves IF)
};

enum class IDTPrivilege : uint8_t {
    Kernel = 0x00,  // Ring 0 only
    User   = 0x60,  // Ring 3 (DPL=3)
};

constexpr uint8_t make_idt_attr(IDTPrivilege priv, IDTGateType gate) {
    return 0x80 | static_cast<uint8_t>(priv) | static_cast<uint8_t>(gate);
}
```

`ExceptionVector` 枚举列出了我们这一章要处理的所有 CPU 异常向量。注意向量 9（Intel 保留）没有列出来，因为我们不处理它。`IDTGateType` 的值是硬件定义的——0x0E 对应 64-bit Interrupt Gate、0x0F 对应 64-bit Trap Gate。`IDTPrivilege` 的值是 DPL 左移 5 位后的结果——DPL=0 就是 0x00，DPL=3 就是 `3 << 5 = 0x60`。

`make_idt_attr` 是一个 constexpr 函数，把 Present 位（0x80）、DPL 和 Gate Type 组合成一个完整的 type_attr 字节。比如 `make_idt_attr(IDTPrivilege::Kernel, IDTGateType::Interrupt)` = `0x80 | 0x00 | 0x0E` = `0x8E`，这是最常见的"内核态中断门"属性值。

接下来是 `InterruptFrame` 和 `IDT` class：

```cpp
struct [[gnu::packed]] InterruptFrame {
    uint64_t r15, r14, r13, r12;
    uint64_t r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rdx;
    uint64_t rcx, rbx, rax;
    uint64_t error_code;
    uint64_t rip, cs, rflags, rsp, ss;
};

class IDT {
public:
    using Handler = void (*)(InterruptFrame*);
    using Stub = void (*)();

    void init();
    void set_handler(ExceptionVector vector, Stub stub,
                     uint16_t selector, uint8_t type_attr, uint8_t ist = 0);

private:
    struct [[gnu::packed]] Entry {
        uint16_t offset_low;
        uint16_t selector;
        uint8_t  ist;
        uint8_t  type_attr;
        uint16_t offset_mid;
        uint32_t offset_high;
        uint32_t reserved;
    };
    static_assert(sizeof(Entry) == 16, "IDT entry must be 16 bytes");

    struct [[gnu::packed]] Pointer {
        uint16_t limit;
        uint64_t base;
    };

    static constexpr uint16_t kMaxEntries = 256;
    Entry entries_[kMaxEntries]{};
    Pointer idtr_{};

    void load();
};

extern IDT g_idt;
```

`InterruptFrame` 的字段顺序需要再强调一次——从上到下必须和汇编里 push 的顺序完全一致。ISR stub 先 push R15（最后被 push 的在最低地址，也就是结构体的第一个字段），然后依次 R14、R13...RAX，然后是 error_code（CPU 压入的或 stub 填零的），最后是 CPU 自动压入的 RIP、CS、RFLAGS、RSP、SS。如果你把这 21 个字段的顺序弄错了一个，整个寄存器 dump 就是乱的。

`IDT` class 内部持有一个 256 项的 Entry 数组（`entries_[256]`）和一个 IDTR Pointer。256 × 16 字节 = 4096 字节 = 恰好一页，这不是巧合——IDT 的总大小正好等于一个 4KB 页，分配和管理都很方便。`entries_` 数组也是零初始化的（放在 BSS 段），零值意味着 Present 位为 0，CPU 不会误用这些未配置的条目。

**验证**：此步完成后编译通过，但还没有可运行的输出。

### 第四步——ISR 汇编 Stub：两个宏搞定全部异常

**目标**：编写两个 GAS 宏 `ISR_NOERRCODE` 和 `ISR_ERRCODE`，分别生成"没有硬件错误码"和"有硬件错误码"的异常处理跳板，然后用这些宏实例化所有 14 个异常的 stub 函数。

这是整个章节里汇编密度最高的一步，但逻辑并不复杂。两种 stub 的唯一区别在于：`ISR_NOERRCODE` 版本需要先 `push $0` 填一个假的错误码（因为 CPU 没有压入错误码，我们需要手动对齐栈帧），而 `ISR_ERRCODE` 版本跳过这一步（CPU 已经压入了错误码）。

**代码**（文件路径：`kernel/arch/x86_64/interrupts.S`）：

先看 `ISR_NOERRCODE` 宏——用于 #DE、#DB、#NMI、#BP、#OF、#BR、#UD、#NM 这 8 个没有硬件错误码的异常：

```asm
.section .text

.macro ISR_NOERRCODE name handler
.global \name
.type \name, @function
\name:
    pushq $0                          # push dummy error code 0

    /* Save all general-purpose registers */
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rbp
    pushq %rsi
    pushq %rdi
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    movq %rsp, %rdi                   # pass InterruptFrame* as first arg
    call \handler                     # call the C handler

    /* Restore all general-purpose registers (reverse order) */
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rdi
    popq %rsi
    popq %rbp
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax

    addq $8, %rsp                     # skip error code
    iretq                             # interrupt return
.endm
```

宏的工作流程是这样的：首先 `pushq $0` 填入假错误码（只有 `ISR_NOERRCODE` 版本有这一步），然后按 RAX → R15 的顺序把所有 15 个通用寄存器压入栈。此时栈指针 RSP 指向的就是完整的 `InterruptFrame` 结构体的起始地址。我们把 RSP 复制到 RDI（System V AMD64 ABI 的第一个参数寄存器），然后 `call` 对应的 C 处理函数。

C 处理函数执行完毕后，我们按反序 pop 所有寄存器（R15 → RAX，和 push 的顺序相反），然后 `addq $8, %rsp` 跳过栈上的错误码（无论真假），最后 `iretq` 从中断返回——CPU 会从栈上弹出 RIP、CS、RFLAGS、RSP、SS，恢复到异常发生前的状态。

`ISR_ERRCODE` 宏几乎一样，唯一区别是少了 `pushq $0` 那一行——因为 CPU 在触发这些异常时已经自动把错误码压入栈了。适用于 #DF、#TS、#NP、#SS、#GP、#PF 这 6 个有硬件错误码的异常。

然后是实例化所有 14 个 stub：

```asm
# Exceptions without error code
ISR_NOERRCODE isr_de_stub,  handle_de       /* #DE(0):  Divide Error */
ISR_NOERRCODE isr_db_stub,  handle_db       /* #DB(1):  Debug */
ISR_NOERRCODE isr_nmi_stub, handle_nmi      /* NMI(2) */
ISR_NOERRCODE isr_bp_stub,  handle_bp       /* #BP(3):  Breakpoint */
ISR_NOERRCODE isr_of_stub,  handle_of       /* #OF(4):  Overflow */
ISR_NOERRCODE isr_br_stub,  handle_br       /* #BR(5):  BOUND Range */
ISR_NOERRCODE isr_ud_stub,  handle_ud       /* #UD(6):  Invalid Opcode */
ISR_NOERRCODE isr_nm_stub,  handle_nm       /* #NM(7):  Device Not Available */

# Exceptions with error code
ISR_ERRCODE   isr_df_stub,  handle_df       /* #DF(8):  Double Fault */
ISR_ERRCODE   isr_ts_stub,  handle_ts       /* #TS(10): Invalid TSS */
ISR_ERRCODE   isr_np_stub,  handle_np       /* #NP(11): Segment Not Present */
ISR_ERRCODE   isr_ss_stub,  handle_ss       /* #SS(12): Stack Fault */
ISR_ERRCODE   isr_gp_stub,  handle_gp       /* #GP(13): General Protection */
ISR_ERRCODE   isr_pf_stub,  handle_pf       /* #PF(14): Page Fault */
```

宏的第一个参数是 stub 的函数名（会被 `idt.cpp` 用 `extern "C"` 声明并引用），第二个参数是 C 处理函数的名字（会被链接器解析到 `exception_handlers.cpp` 里的对应函数）。比如 `ISR_NOERRCODE isr_bp_stub, handle_bp` 会生成一个名为 `isr_bp_stub` 的汇编函数，它的 body 调用 C 函数 `handle_bp`。

这里有一个非常容易踩的坑——哪些异常有硬件错误码、哪些没有，不能记错。#DF（向量 8，Double Fault）是有错误码的（虽然错误码通常为 0），#BR（向量 5，BOUND Range Exceeded）没有错误码，#TS（向量 10）有错误码。如果你把一个有错误码的异常用 `ISR_NOERRCODE` 宏处理了，栈上就会多出一个 CPU 压入的错误码，IRETQ 的时候弹出的 RIP 实际上是错误码的值，CPU 会跳到一个随机地址执行——这种 bug 非常难定位，因为现象是"IRETQ 之后崩溃"而不是"异常处理时崩溃"。

**验证**：此步完成后编译通过，但还没有可运行的输出。

### 第五步——IDT 初始化：数据驱动的路由表 + 循环注册

**目标**：实现 `IDT::init()` 用一个数据驱动的路由表（Route struct 数组）来配置所有异常向量，然后 `lidt` 加载 IDTR。相比为每个异常写一遍重复的 `set_handler` 调用，路由表方式更不容易遗漏某个向量，修改起来也更方便。

**代码**（文件路径：`kernel/arch/x86_64/idt.cpp`）：

先是在文件顶部声明 ISR stub 和 C handler 的 extern "C" 链接（因为它们分别定义在 `interrupts.S` 和 `exception_handlers.cpp` 里，用的是 C 语言的命名规则）：

```cpp
#include "kernel/arch/x86_64/idt.hpp"
#include "kernel/arch/x86_64/gdt.hpp"
#include <stdint.h>

namespace cinux::arch {

IDT g_idt;

extern "C" {
void isr_de_stub();
void isr_db_stub();
// ... 所有 14 个 stub 的声明 ...
}  // extern "C"

extern "C" {
void handle_de(InterruptFrame*);
void handle_db(InterruptFrame*);
// ... 所有 14 个 handler 的声明 ...
}  // extern "C"
```

然后是 `set_handler` 和 `init` 的实现：

```cpp
void IDT::set_handler(ExceptionVector vector, Stub stub,
                      uint16_t selector, uint8_t type_attr, uint8_t ist) {
    const auto vec  = static_cast<uint8_t>(vector);
    const auto addr = reinterpret_cast<uint64_t>(stub);

    entries_[vec].offset_low  = static_cast<uint16_t>(addr & 0xFFFF);
    entries_[vec].offset_mid  = static_cast<uint16_t>((addr >> 16) & 0xFFFF);
    entries_[vec].offset_high = static_cast<uint32_t>((addr >> 32) & 0xFFFFFFFF);
    entries_[vec].selector    = selector;
    entries_[vec].ist         = ist;
    entries_[vec].type_attr   = type_attr;
    entries_[vec].reserved    = 0;
}
```

`set_handler` 把一个 64 位地址拆成三段（低 16 位、中间 16 位、高 32 位），分别填入 IDT Entry 的三个 offset 字段。这是 64 位 IDT 的标准做法——16 + 16 + 32 = 64 位完整地址。`selector` 是代码段选择子（我们传 `GDT_KERNEL_CODE = 0x08`），表示中断处理程序运行在内核代码段。`ist` 是 IST 偏移（我们目前全部传 0，不使用 IST 切换栈）。

```cpp
void IDT::init() {
    // Clear all entries
    for (auto& entry : entries_) {
        entry = Entry{};
    }

    // Data-driven exception routing
    struct Route {
        ExceptionVector vector;
        Stub stub;
        IDTPrivilege priv;
        IDTGateType gate;
    };

    const Route routes[] = {
        {ExceptionVector::DE,  isr_de_stub,  IDTPrivilege::Kernel, IDTGateType::Interrupt},
        {ExceptionVector::DB,  isr_db_stub,  IDTPrivilege::Kernel, IDTGateType::Trap},
        {ExceptionVector::NMI, isr_nmi_stub, IDTPrivilege::Kernel, IDTGateType::Interrupt},
        {ExceptionVector::BP,  isr_bp_stub,  IDTPrivilege::User,   IDTGateType::Trap},
        {ExceptionVector::OF,  isr_of_stub,  IDTPrivilege::Kernel, IDTGateType::Interrupt},
        {ExceptionVector::BR,  isr_br_stub,  IDTPrivilege::Kernel, IDTGateType::Interrupt},
        {ExceptionVector::UD,  isr_ud_stub,  IDTPrivilege::Kernel, IDTGateType::Interrupt},
        {ExceptionVector::NM,  isr_nm_stub,  IDTPrivilege::Kernel, IDTGateType::Interrupt},
        {ExceptionVector::DF,  isr_df_stub,  IDTPrivilege::Kernel, IDTGateType::Interrupt},
        {ExceptionVector::TS,  isr_ts_stub,  IDTPrivilege::Kernel, IDTGateType::Interrupt},
        {ExceptionVector::NP,  isr_np_stub,  IDTPrivilege::Kernel, IDTGateType::Interrupt},
        {ExceptionVector::SS,  isr_ss_stub,  IDTPrivilege::Kernel, IDTGateType::Interrupt},
        {ExceptionVector::GP,  isr_gp_stub,  IDTPrivilege::Kernel, IDTGateType::Interrupt},
        {ExceptionVector::PF,  isr_pf_stub,  IDTPrivilege::Kernel, IDTGateType::Interrupt},
    };

    for (const auto& r : routes) {
        set_handler(r.vector, r.stub, GDT_KERNEL_CODE,
                    make_idt_attr(r.priv, r.gate), 0);
    }

    idtr_.limit = static_cast<uint16_t>(sizeof(entries_) - 1);
    idtr_.base  = reinterpret_cast<uint64_t>(entries_);

    load();
}

void IDT::load() {
    __asm__ volatile("lidt %[idtr]\n\t"
                     : : [idtr] "m"(idtr_) : "memory");
}
```

路由表的设计是这一章一个比较用心的地方。你可以看到 #BP 和 #DB 的 gate type 是 `Trap`（允许中断继续），其他全部是 `Interrupt`（关中断）；#BP 的 privilege 是 `User`（允许 Ring 3 触发），其他全部是 `Kernel`（只有 Ring 0 能触发）。这些策略决策集中在一个表里表达，比分散在 14 个独立的 `set_handler` 调用里要清晰得多——以后要修改某个向量的配置，只需要改表里的一行。

`lidt` 指令和 `lgdt` 类似，从内存中读取 10 字节数据（2 字节 limit + 8 字节 base）加载到 CPU 的 IDTR 寄存器。之后 CPU 在处理异常时就会用这个 IDTR 来查找 IDT。注意 `IDT::init()` 必须在 `GDT::init()` 之后调用，因为 IDT 条目里的 selector 引用了 GDT 中的代码段描述符——如果 GDT 还没加载，CPU 在触发异常时查 IDT 得到了 selector 0x08，再去 GDT 查对应的描述符，结果 GDT 里还是空的，直接 triple fault。

**验证**：此步完成后编译通过，但还没有可运行的输出。

### 第六步——异常处理函数：dump_registers + 致命 halt

**目标**：实现所有 14 个 C 异常处理函数，包括一个通用的 `dump_registers` 函数来打印完整的寄存器 dump，以及 `fatal_halt` 函数在致命异常后进入死循环。

**代码**（文件路径：`kernel/arch/x86_64/exception_handlers.cpp`）：

首先是两个辅助函数，放在匿名命名空间里（只在当前编译单元可见）：

```cpp
#include "kernel/arch/x86_64/idt.hpp"
#include "kernel/lib/kprintf.hpp"
#include <stdint.h>

namespace {
using cinux::arch::InterruptFrame;
using cinux::lib::kprintf;

void dump_registers(const InterruptFrame* frame,
                    const char* name, uint8_t vector) {
    kprintf("\n");
    kprintf("==== EXCEPTION: %s (vector %u) ====\n", name, vector);
    kprintf("  RIP   = %p   CS  = 0x%04x\n",
            reinterpret_cast<void*>(frame->rip),
            static_cast<unsigned>(frame->cs));
    kprintf("  RFLAGS= %p\n",
            reinterpret_cast<void*>(frame->rflags));
    kprintf("  RSP   = %p   SS  = 0x%04x\n",
            reinterpret_cast<void*>(frame->rsp),
            static_cast<unsigned>(frame->ss));
    kprintf("  RAX=%p  RBX=%p\n",
            reinterpret_cast<void*>(frame->rax),
            reinterpret_cast<void*>(frame->rbx));
    kprintf("  RCX=%p  RDX=%p\n",
            reinterpret_cast<void*>(frame->rcx),
            reinterpret_cast<void*>(frame->rdx));
    kprintf("  RSI=%p  RDI=%p\n",
            reinterpret_cast<void*>(frame->rsi),
            reinterpret_cast<void*>(frame->rdi));
    kprintf("  RBP=%p  R8 =%p\n",
            reinterpret_cast<void*>(frame->rbp),
            reinterpret_cast<void*>(frame->r8));
    kprintf("  R9 =%p  R10=%p\n",
            reinterpret_cast<void*>(frame->r9),
            reinterpret_cast<void*>(frame->r10));
    kprintf("  R11=%p  R12=%p\n",
            reinterpret_cast<void*>(frame->r11),
            reinterpret_cast<void*>(frame->r12));
    kprintf("  R13=%p  R14=%p\n",
            reinterpret_cast<void*>(frame->r13),
            reinterpret_cast<void*>(frame->r14));
    kprintf("  R15=%p\n",
            reinterpret_cast<void*>(frame->r15));
    kprintf("  ERROR CODE = %p\n",
            reinterpret_cast<void*>(frame->error_code));
    kprintf("========================================\n");
}

[[noreturn]] void fatal_halt() {
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}

}  // anonymous namespace
```

`dump_registers` 接收一个 `InterruptFrame*` 指针，把所有寄存器值通过 `kprintf` 打印到串口。这里用 `%p` 格式来打印 64 位指针值（我们的 `kprintf` 实现会输出 `0x` 前缀加 16 位十六进制数字），CS 和 SS 用 `%04x` 格式打印（因为它们是 16 位段选择子）。异常名和向量号作为额外参数传入，这样每个 handler 都能打印出自己的名字。

`fatal_halt` 是一个永远不会返回的函数——`[[noreturn]]` 属性告诉编译器这一点，编译器可以在调用它的代码之后省略掉不可达的代码。函数体内是一个死循环，每次循环先 `cli`（关中断）再 `hlt`（暂停 CPU 直到下一个中断到来），但因为已经 `cli` 了所以不会有中断到来——CPU 永远停在这里。如果发生 NMI（不可屏蔽中断），CPU 会短暂醒来，但 `jmp` 回循环开头再次 `cli; hlt`。

然后是各异常处理函数。非致命异常（#BP、#DB）打印 dump 后直接返回，ISR stub 会通过 IRETQ 恢复执行：

```cpp
extern "C" {

void handle_bp(InterruptFrame* frame) {
    dump_registers(frame, "#BP", 3);
    kprintf("[EXCEPTION] Breakpoint at RIP=%p\n",
            reinterpret_cast<void*>(frame->rip));
    kprintf("[EXCEPTION] Continuing...\n");
}

void handle_db(InterruptFrame* frame) {
    dump_registers(frame, "#DB", 1);
    kprintf("[EXCEPTION] Debug exception, continuing...\n");
}
```

致命异常的处理函数打印 dump 后调用 `fatal_halt()`：

```cpp
void handle_de(InterruptFrame* frame) {
    dump_registers(frame, "#DE", 0);
    kprintf("[FATAL] Divide Error -- halting.\n");
    fatal_halt();
}

void handle_gp(InterruptFrame* frame) {
    dump_registers(frame, "#GP", 13);
    kprintf("[FATAL] General Protection Fault (error code=%p) -- halting.\n",
            reinterpret_cast<void*>(frame->error_code));
    fatal_halt();
}
```

#PF（页错误）的处理函数要特殊一些——它需要从 CR2 寄存器读出触发缺页的虚拟地址，并且把错误码的各个 bit 拆解成可读的字符串：

```cpp
void handle_pf(InterruptFrame* frame) {
    uint64_t fault_addr;
    __asm__ volatile("movq %%cr2, %0" : "=r"(fault_addr));

    uint64_t err = frame->error_code;
    const char* present  = (err & 0x01) ? "protection violation" : "page not present";
    const char* access   = (err & 0x02) ? "write" : "read";
    const char* mode     = (err & 0x04) ? "user" : "kernel";
    const char* reserved = (err & 0x08) ? ", reserved bits" : "";
    const char* fetch    = (err & 0x10) ? ", instruction fetch" : "";

    dump_registers(frame, "#PF", 14);
    kprintf("[FATAL] Page Fault: %s %s %s%s%s\n",
            present, access, mode, reserved, fetch);
    kprintf("[FATAL] Faulting address (CR2) = %p -- halting.\n",
            reinterpret_cast<void*>(fault_addr));
    fatal_halt();
}

}  // extern "C"
```

CR2 是 x86 的页错误线性地址寄存器——CPU 在触发 #PF 时会自动把触发缺页的那个虚拟地址写入 CR2。我们用 inline assembly `movq %%cr2, %0` 读出这个值，然后和错误码的解析结果一起打印。错误码的 bit 0 表示是"页不存在"（0）还是"权限违反"（1），bit 1 表示是读操作（0）还是写操作（1），bit 2 表示是内核态（0）还是用户态（1），bit 3 表示保留位是否被设置了，bit 4 表示是否是指令取址触发的。这些信息对于调试页表相关问题极其有用。

**验证**：此步完成后编译通过，但还没有可运行的输出。

### 第七步——kernel_main 里串起来：GDT → IDT → int $3 测试

**目标**：在 `kernel_main` 里按正确顺序调用 `g_gdt.init()` 和 `g_idt.init()`，然后用 `int $3` 触发断点异常来验证整个异常处理链路能正常工作。

**代码**（文件路径：`kernel/main.cpp`）：

```cpp
#include <stdint.h>

#include "kernel/arch/x86_64/gdt.hpp"
#include "kernel/arch/x86_64/idt.hpp"
#include "kernel/lib/kprintf.hpp"

extern "C" void kernel_main() {
    // Step 1: Initialise serial port for kprintf
    cinux::lib::kprintf_init();

    cinux::lib::kprintf("[BIG] Big kernel running @ 0x1000000\n");

    // Step 2: Initialise GDT (must come before IDT)
    cinux::arch::g_gdt.init();
    cinux::lib::kprintf("[BIG] GDT loaded.\n");

    // Step 3: Initialise IDT (depends on GDT selectors)
    cinux::arch::g_idt.init();
    cinux::lib::kprintf("[BIG] IDT loaded.\n");

    // Step 4: Trigger a software breakpoint to test exception handling
    cinux::lib::kprintf("[BIG] Triggering int $3 breakpoint...\n");
    __asm__ volatile("int $3");
    cinux::lib::kprintf("[BIG] Breakpoint returned, continuing.\n");

    // Halt
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}
```

初始化顺序是关键——必须先 GDT 后 IDT，原因前面已经解释过了（IDT 条目引用 GDT 中的段选择子）。串口初始化（`kprintf_init`）必须在最前面，因为后续的 `kprintf` 调用都依赖串口驱动。整个流程是：串口 → GDT → IDT → 测试 → halt。

`int $3` 是 x86 的软件断点指令，它会触发向量 3（#BP）的异常。我们之前在 IDT 路由表里把 #BP 配成了 Trap Gate + User DPL，所以这条指令在 Ring 0 下可以正常触发。ISR stub（`isr_bp_stub`）会保存寄存器、调用 `handle_bp` C 函数，`handle_bp` 打印完寄存器 dump 后返回，ISR stub 通过 IRETQ 恢复执行——所以 `int $3` 之后的 `kprintf("[BIG] Breakpoint returned, continuing.\n")` 会被正常执行。

这里有一个非常重要的设计决策——在 `int $3` 测试之前我们没有调用 `sti`（开中断）。这是因为虽然我们配置了 14 个 CPU 异常向量的 IDT 条目，但还没有配置任何硬件 IRQ 的处理程序（定时器、键盘等）。如果此时 `sti` 开中断，PIT 定时器（默认频率约 18.2Hz）会立刻产生一个 IRQ0 中断，CPU 查 IDT[32] 发现是空的（Present=0），触发 #GP，然后 #GP 的处理函数 `fatal_halt`——内核直接死掉。这不是 bug，而是我们故意的设计选择：先只处理 CPU 异常，硬件中断的处理留到后面的章节。

**验证**：这是我们可以第一次看到完整输出的步骤。构建并运行后，串口应该输出如下内容：

```
[BIG] Big kernel running @ 0x1000000
[BIG] GDT loaded.
[BIG] IDT loaded.
[BIG] Triggering int $3 breakpoint...

==== EXCEPTION: #BP (vector 3) ====
  RIP   = 0x...    CS  = 0x0008
  RFLAGS= 0x...
  RSP   = 0x...    SS  = 0x0010
  RAX=0x...  RBX=0x...
  ...（所有通用寄存器）...
  ERROR CODE = 0x0000000000000000
========================================
[EXCEPTION] Breakpoint at RIP=0x...
[EXCEPTION] Continuing...
[BIG] Breakpoint returned, continuing.
```

---

## 构建与运行

现在我们来构建并运行，看看大内核的异常处理是否真正工作。

```bash
# 从项目根目录
git checkout 010_big_kernel_gdt_idt

# 配置 + 构建（Debug 模式，带符号信息）
cmake -B build -DCMAKE_BUILD_TYPE=Debug -S .
cmake --build build -j$(nproc)

# 运行
cd build
make run
```

`make run` 会调用 `scripts/build_image.sh` 构建磁盘镜像，然后启动 QEMU。QEMU 的启动参数含义如下：

- `-m 8G`：分配 8GB 内存
- `-serial stdio`：把串口输出重定向到终端——我们的 `kprintf` 就是往串口写的，所以终端上直接能看到内核输出
- `-debugcon file:debug.log`：把 I/O 端口 0xE9 的输出写到 `debug.log` 文件——这是 QEMU 的调试控制台，可以用来辅助调试
- `-no-reboot -no-shutdown`：内核异常时不自动重启或关机——Triple Fault 后 QEMU 会停在 "Shutdown" 状态而不是直接重启，方便我们查看最后的串口输出
- `-device isa-debug-exit,iobase=0xf4,iosize=0x04`：用于内核测试的自动退出设备——往 0xF4 端口写特定值会让 QEMU 以退出码退出

正常运行的串口输出应该是这样的：首先 `[BIG]` 系列消息逐行出现，然后 `int $3` 触发后打印出完整的 EXCEPTION 块，接着 `Breakpoint returned, continuing.` 表示 IRETQ 成功恢复了执行，最后内核进入 halt 循环（QEMU 停在那里不动了）。

---

## 调试技巧

### Triple Fault 排查：GDT/IDT 加载顺序搞错了

这是这一章最常见的 bug。如果你看到的现象是 QEMU 直接重启（或者因为 `-no-reboot` 而停在 "Shutdown" 状态），没有任何串口输出，那大概率是 IDT 加载时引用了错误的 GDT 选择子，或者 GDT 还没加载就先加载了 IDT。

排查方法：在 `kernel_main` 里每一行 `init()` 之后都加一个 `kprintf`，这样能看到最后一条成功打印的消息。如果输出停在 `[BIG] GDT loaded.` 之后，说明 IDT 的 `init()` 或 `lidt` 触发了异常——这时候很可能是因为 IDT init 代码本身有 bug（比如 `set_handler` 写入了错误的向量号），或者 GDT 的 code64 描述符配错了。用 GDB 连上去看看：

```bash
# 终端 1
cd build && make run-debug

# 终端 2
gdb -ix scripts/.gdbinit
# 在 GDB 里
(gdb) break kernel_main
(gdb) continue
(gdb) stepi   # 单步执行 init 调用
```

### ISR stub 的寄存器保存顺序和 InterruptFrame 不一致

如果寄存器 dump 打印出来的值看起来完全不合理（比如 RSP 是一个奇数地址，或者 RIP 是一个很小的数字），那很可能是 `InterruptFrame` 结构体的字段顺序和汇编里的 push 顺序对不上。这种 bug 不会导致崩溃（因为 IRETQ 恢复的是 CPU 自动压入的那 5 个值，不是我们 push 的通用寄存器），但打印出来的信息全是错的。

排查方法：在 `int $3` 之前把一个已知值放到某个寄存器里（比如 `movq $0xDEADBEEF, %rax`），然后看 dump 里 RAX 是不是这个值。如果 RAX 字段显示的不是 `0xDEADBEEF`，而是其他寄存器的值，那说明结构体字段偏移有问题。

### 错误码的有无搞反了

如果把一个有硬件错误码的异常（比如 #GP）用 `ISR_NOERRCODE` 宏处理了，栈上就会多出 CPU 压入的那个错误码——IRETQ 的时候弹出的不是真正的 RIP 而是错误码的值，CPU 跳到一个非法地址执行，然后再次触发异常，进入无限循环直到 QEMU 的 `-no-reboot` 让它停下来。

反过来，如果把一个没有硬件错误码的异常（比如 #BP）用 `ISR_ERRCODE` 宏处理了，栈上会少 8 字节——ISR stub 以为 CPU 已经 push 了错误码所以没有 push `$0`，但实际上 CPU 没有 push，于是 stub 保存的 R15 其实覆盖了 CPU 压入的 RIP 值，IRETQ 的时候恢复的也是错误的地址。

排查方法：在 `dump_registers` 里看 ERROR CODE 的值——对于没有硬件错误码的异常，如果 ERROR CODE 不是 0，那很可能是有无搞反了。但这个方法不太可靠，因为有些有错误码的异常错误码恰好就是 0（比如 #DF）。最靠谱的方法是查 Intel SDM 的 Table 6-1（"Protected-Mode Exceptions and Interrupts"），确认每个异常是否有错误码。

---

## 本章小结

| 类别 | 名称 | 说明 |
|------|------|------|
| 类 | `cinux::arch::GDT` | GDT 封装：7 条目数组 + GDTR + TSS + lgdt/ltr |
| 类 | `cinux::arch::IDT` | IDT 封装：256 条目数组 + IDTR + lidt |
| 枚举 | `SegmentAccess` | GDT access byte 的位域标志（Present/Ring/Executable/...）|
| 枚举 | `SegmentFlags` | GDT flags byte 的位域标志（Granularity/LongMode/Size32）|
| 枚举 | `ExceptionVector` | CPU 异常向量号（DE=0, BP=3, GP=13, PF=14, ...）|
| 枚举 | `IDTGateType` | IDT 门类型（Interrupt=0xE, Trap=0xF）|
| 枚举 | `IDTPrivilege` | IDT DPL（Kernel=0, User=3）|
| 结构体 | `InterruptFrame` | 中断栈帧：15 个通用寄存器 + error_code + CPU 5 元组 |
| 结构体 | `TaskStateSegment` | 64 位 TSS 占位（104 字节）|
| 宏 | `ISR_NOERRCODE` | 无错误码异常的 ISR stub 生成宏 |
| 宏 | `ISR_ERRCODE` | 有错误码异常的 ISR stub 生成宏 |
| 函数 | `dump_registers` | 打印完整寄存器 dump 到串口 |
| 函数 | `fatal_halt` | 致命异常后的 cli;hlt 死循环 |
| 函数 | `make_idt_attr` | 从 DPL + Gate Type 生成 IDT type_attr 字节 |
| 寄存器 | CR2 | 页错误线性地址（#PF 时 CPU 自动写入触发地址）|
| 选择子 | 0x08/0x10/0x1B/0x23/0x28 | GDT 中 Kernel Code/Data、User Code/Data、TSS 的选择子 |

本章我们从零搭建了大内核的异常处理基础设施——GDT 提供段寄存器配置，IDT 提供异常分发，ISR stub 提供汇编到 C 的桥接，exception_handlers 提供具体的处理逻辑（非致命异常打印后继续，致命异常打印后 halt）。触发 `int $3` 后能看到完整的寄存器 dump 并且程序继续执行，这就验证了整条链路的正确性。

下一章我们会把硬件中断（IRQ）也接进来——配置 8259A PIC 或者 APIC，处理定时器中断和键盘中断，让内核能够真正地响应外部事件。到那时候我们就需要 `sti` 开中断了，但前提是所有 IRQ 向量的 IDT 条目都配好了处理程序，不然开中断那一刻就是 triple fault 的那一刻。