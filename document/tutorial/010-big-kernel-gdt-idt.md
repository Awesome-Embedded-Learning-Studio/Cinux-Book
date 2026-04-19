# 从零在 x64 内核中搭建 GDT/IDT/中断系统：用现代 C++ 封装硬件描述符表的完整实录

> 作者：
> 标签：x86-64, GDT, IDT, 中断处理, ISR, C++ 内核, freestanding, AT&T 汇编, QEMU, 裸机开发

---

## 前言

如果你跟我一样，是从 mini kernel 一路折腾上来的，那你大概率还记得 milestone 007 里我们给 mini kernel 装过一套简易版的中断系统——GDT 只有三个描述符（null + code + data），IDT 只挂了两个向量（#BP 和 #PF），ISR stub 用内联汇编拼凑，C handler 直接裸奔。那套方案在 mini kernel 阶段够用，因为它只是一个"能启动、能跑、不死机就算成功"的验证平台。但现在我们站在大内核的门槛上了——大内核有完整的链接脚本、higher-half 地址空间、freestanding C++ 运行时，未来还要承载文件系统、进程管理、网络协议栈这些重型基础设施。如果继续沿用 mini kernel 那套 C 风格全局函数 + 硬编码魔数的中断实现，代码很快就会变成维护噩梦。

所以 milestone 010 的目标非常明确：在大内核里从零重新搭建一套完整的中断基础设施。GDT 要配齐五个段描述符（kernel code、kernel data、user code、user data、TSS），IDT 要覆盖 Intel 手册定义的前 15 个 CPU 异常向量（0 到 14），ISR stub 要用正规的 AT&T 汇编宏来生成，C handler 要能把异常分成"致命"和"非致命"两种策略处理。最关键的是，整个实现要用 C++ class 封装——`GDT` 和 `IDT` 各自是一个类，描述符表的生命周期和操作都绑在对象上，不再是散落的全局函数和 `extern` 变量。这样做不是为了"显得高级"，而是因为我们后面还要支持多架构扩展（比如未来加 ARM64），class 封装能让每种架构的描述符表管理逻辑独立演化，不会互相污染。

最终的验证标准也很直观：在大内核的 `kernel_main` 里用 `int $3` 触发一个软件断点异常，QEMU 串口输出完整的寄存器 dump，然后内核不死机、不重启，继续往下执行。看起来简单，但背后是 GDT → IDT → ISR stub → C handler → IRETQ 这一整条链路的完整打通。

## 环境说明

实验环境和之前几个 milestone 保持一致：x86_64 平台，GNU AS + GCC/G++ + CMake 构建，QEMU 模拟运行。大内核仍然是 freestanding C++23，编译标志用 `-mcmodel=large`（因为大内核链接到 higher-half 地址空间，需要 64 位绝对寻址），其余约束照旧——无标准库、无异常、无 RTTI、无栈保护、无红区。

内存布局方面需要回顾几个关键参数。大内核被 mini kernel 加载到物理地址 `0x1000000`（16MB），运行在 higher-half 虚拟地址 `0xFFFFFFFF80000000`。当前阶段 mini kernel 只建立了 identity mapping，所以大内核实际上还在物理地址上执行。GDT 和 IDT 的实例被声明为全局 BSS 变量，零初始化后由 `init()` 函数在运行时填充。

一个需要特别强调的约束是 `-mno-red-zone`。在 x86_64 System V ABI 中，函数可以使用栈顶以下 128 字节的"red zone"作为临时空间而不需要调整 RSP——这在用户态应用中是个不错的优化，但在内核里是定时炸弹。因为中断可能在任何时刻打断执行，ISR 的栈操作会直接覆盖 red zone 里的数据。这个编译选项在内核开发中是强制的，不能省。

## 第一步——搞清楚我们要搭什么

在开始写代码之前，我们先从宏观上看一下整个中断系统的架构。这次要搭的东西比 mini kernel 版本复杂不少，理解清楚再动手能省掉后面很多调试时间。

GDT（全局描述符表）需要七个槽位。第一个是 null descriptor，这是 x86 架构的硬性要求，索引 0 的描述符永远不能被引用。第二到第五个分别是 kernel code segment、kernel data segment、user code segment、user data segment——前两个给 ring 0 内核态用，后两个给 ring 3 用户态用（虽然我们当前还在 ring 0，但先把描述符填好，后续实现用户态进程时就不需要再回来改 GDT 了）。第六和第七个是 TSS（Task State Segment）描述符——在 x86_64 中，TSS 描述符占两个 GDT 槽位（共 16 字节），因为 64 位模式下 TSS 的基地址是 64 位的，一个标准 8 字节描述符放不下，需要用"低 32 位 + 高 32 位"的两段式编码。

IDT（中断描述符表）这次要覆盖 15 个异常向量。Intel SDM 定义的 CPU 异常从 #DE（向量 0，除零错误）到 #PF（向量 14，页错误），中间跳过了向量 9（在 64 位模式下不使用）。其中向量 8、10、11、12、13、14 这六个会由 CPU 自动压入错误码，其余的不会——ISR stub 需要区分这两种情况，我们后面会详细讲。门类型方面，我们采用混合策略：#BP（向量 3）和 #DB（向量 1）使用陷阱门（Trap Gate），其余全部使用中断门（Interrupt Gate）。原因很简单——陷阱门不会清除 RFLAGS.IF（不关中断），适合调试类异常；中断门会自动关中断，适合其他所有可能导致系统状态不一致的异常。#BP 的 DPL 还要设为 3（用户态可触发），这样 `int $3` 指令才能从 ring 3 执行。

ISR stub 是纯汇编写的，定义在 `interrupts.S` 中。核心思路是两个宏：`ISR_NOERRCODE` 给不产生硬件错误码的异常用（先 push 一个伪错误码占位），`ISR_ERRCODE` 给有硬件错误码的异常用（CPU 已经 push 过了，不需要额外操作）。两个宏都会保存全部 16 个通用寄存器，然后把栈指针传给 C handler，处理完后恢复寄存器、跳过错误码、IRETQ 返回。这套机制和 mini kernel 版本本质上是一样的，只是覆盖的向量从 2 个扩展到了 15 个。

## 第二步——用 C++ class 封装 GDT

现在我们来写代码。先从 GDT 开始，因为 IDT 依赖 GDT 中的代码段选择子——初始化顺序必须是 GDT 先、IDT 后。

GDT 的类定义在 `kernel/arch/x86_64/gdt.hpp` 中。我们先把段选择子常量和描述符标志位定义出来，这些是构建描述符条目的基础材料：

```cpp
namespace cinux::arch {

constexpr uint16_t GDT_KERNEL_CODE = 0x08;
constexpr uint16_t GDT_KERNEL_DATA = 0x10;
constexpr uint16_t GDT_USER_CODE   = 0x1B;
constexpr uint16_t GDT_USER_DATA   = 0x23;
constexpr uint16_t GDT_TSS         = 0x28;
```

段选择子的值 = (索引 << 3) | RPL。`GDT_KERNEL_CODE` 是 0x08，即索引 1、RPL=0；`GDT_KERNEL_DATA` 是 0x10，即索引 2、RPL=0。`GDT_USER_CODE` 是 0x1B，即索引 3、RPL=3——注意末尾的 0x03，这是用户态的请求特权级。`GDT_TSS` 是 0x28，即索引 5，因为 TSS 从 GDT 的第 5 个槽位开始（占用索引 5 和 6 两个槽位）。

接下来是两个 scoped enum，分别表示描述符的 access byte 和 flags 字段：

```cpp
enum class SegmentAccess : uint8_t {
    Present    = 1u << 7,
    Ring0      = 0u << 5,
    Ring3      = 3u << 5,
    CodeData   = 1u << 4,
    Executable = 1u << 3,
    ReadWrite  = 1u << 1,
    TSS64Avail = 0x09,
};

enum class SegmentFlags : uint8_t {
    Granularity4K = 1u << 3,
    LongMode      = 1u << 1,
    Size32        = 1u << 2,
};
```

用 scoped enum 而不是 `#define` 或 `constexpr int` 有几个好处。首先是类型安全——你不能意外把 `SegmentFlags` 的值传给期望 `SegmentAccess` 的函数，编译器会直接报错。其次是可读性——`SegmentAccess::Present | SegmentAccess::Ring0 | SegmentAccess::CodeData` 比 `0x90 | 0x00 | 0x10` 之类的魔数拼凑清晰太多。我们还给两个 enum 各重载了 `operator|`，这样可以用位或运算组合标志位，写法和传统位运算一样自然。

然后是 GDT 类本身：

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
```

`Entry` 是标准的 8 字节段描述符布局，严格按照 Intel SDM 的格式排列字段。`Pointer` 是 GDTR 寄存器的内存布局——2 字节 limit + 8 字节 base（64 位模式下 base 是 64 位的）。`TaskStateSegment` 是 64 位模式下的最小 TSS 结构，104 字节，包含了 RSP0/RSP1/RSP2（三个特权级的栈指针）和 IST1-IST7（七个中断栈表指针）。当前 milestone 我们只是把 TSS 填进 GDT、用 `ltr` 加载 TR，不设置任何具体的栈切换值——完整的 TSS 配置（比如给 double fault 设置 IST）留到后面实现中断栈表的时候再做。

类里还定义了三个 `constexpr` 工厂函数来生成描述符：`null_entry()` 返回全零的 null 描述符，`segment_entry()` 根据 access 和 flags 参数生成代码段或数据段描述符，`tss_low_entry()` 和 `tss_high_entry()` 分别生成 TSS 描述符的低 8 字节和高 8 字节。把这些逻辑封装成 constexpr 函数而不是在 `init()` 里手写字节赋值，好处是可以在编译期验证描述符编码的正确性——如果你改了工厂函数的实现导致描述符格式错误，`static_assert` 会立刻捕获。

成员变量方面，`entries_[7]` 存放 7 个描述符（5 个段 + TSS 占 2 个槽），`gdtr_` 存放 GDTR 指针，`tss_` 是 TSS 结构体实例。全部放在 BSS 段，零初始化，运行时由 `init()` 填充。

接下来看 `init()` 的实现：

```cpp
void GDT::init() {
    entries_[0] = null_entry();

    entries_[1] = segment_entry(
        SegmentAccess::Present | SegmentAccess::Ring0 |
        SegmentAccess::CodeData | SegmentAccess::Executable | SegmentAccess::ReadWrite,
        SegmentFlags::Granularity4K | SegmentFlags::LongMode);

    entries_[2] = segment_entry(
        SegmentAccess::Present | SegmentAccess::Ring0 |
        SegmentAccess::CodeData | SegmentAccess::ReadWrite,
        SegmentFlags::Granularity4K | SegmentFlags::Size32);

    entries_[3] = segment_entry(
        SegmentAccess::Present | SegmentAccess::Ring3 |
        SegmentAccess::CodeData | SegmentAccess::Executable | SegmentAccess::ReadWrite,
        SegmentFlags::Granularity4K | SegmentFlags::LongMode);

    entries_[4] = segment_entry(
        SegmentAccess::Present | SegmentAccess::Ring3 |
        SegmentAccess::CodeData | SegmentAccess::ReadWrite,
        SegmentFlags::Granularity4K | SegmentFlags::Size32);
```

你会发现这段代码读起来几乎像自然语言。索引 1 是 kernel code segment——Present（存在）、Ring0（内核态）、CodeData（代码/数据段类型）、Executable（可执行）、ReadWrite（可读），flags 设为 4K 粒度 + Long Mode。索引 3 是 user code segment，和 kernel code 唯一的区别是 Ring3。这里有一个非常容易踩的坑：在 x86_64 的 64 位代码段中，flags 的 LongMode 位必须为 1，而 Size32 位必须为 0——如果你两个都设了 1，CPU 会认为这是一个 32 位兼容模式段而不是 64 位段，执行时直接触发 General Protection Fault。数据段则反过来，flags 用 Size32 而不是 LongMode——Intel 手册规定 64 位数据段的 L 位和 D 位都被硬件忽略，但惯例是用 Size32。

TSS 描述符的设置需要特殊处理，因为它在 x86_64 中跨越两个 GDT 槽位：

```cpp
    const auto tss_addr = reinterpret_cast<uint64_t>(&tss_);
    entries_[5] = tss_low_entry(tss_addr, sizeof(TaskStateSegment) - 1);
    entries_[6] = tss_high_entry(tss_addr);
```

`tss_low_entry` 把 TSS 的 64 位基地址拆成低 32 位，填入标准描述符格式的 base 字段；`tss_high_entry` 把高 32 位填入第二个 8 字节槽位的对应位置。limit 设为 `sizeof(TaskStateSegment) - 1`，即 103——这是 TSS 的标准大小减一（limit 是"最大有效偏移量"的含义）。access byte 设为 `Present | TSS64Avail`，其中 `TSS64Avail = 0x09` 表示"64 位可用的 TSS 描述符"。

描述符填好后，构建 GDTR 并加载：

```cpp
    gdtr_.limit = sizeof(entries_) - 1;
    gdtr_.base  = reinterpret_cast<uint64_t>(entries_);

    load();
}
```

`load()` 函数是整个 GDT 初始化中最"危险"的部分，因为它涉及内联汇编和段寄存器刷新：

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

`lgdt` 把 GDTR 加载到 CPU，但此时 CS 缓存里还是旧的段描述符。我们需要一次"远返回"来强制刷新 CS——先把新的 CS 选择子（0x08）和返回地址压栈，然后 `lretq` 从栈上弹出 RIP 和 CS，完成段寄存器切换。DS/ES/FS/GS/SS 不需要这种花活，直接用 `mov` 赋值就行。最后 `ltr` 加载 TR 寄存器，指向 TSS 描述符——到这里 GDT 就完全配好了。

这里有一个坑值得提前说一下：`ltr` 指令要求操作数是一个通用寄存器，不能是立即数。所以 `const uint16_t tss_sel = GDT_TSS;` 这行看似多余——为什么不直接把 `GDT_TSS` 塞进汇编？原因是 `ltr` 的约束是 `"r"`（register），需要一个变量绑到寄存器上。如果直接用 `"i"`（immediate），编译器会报内联汇编约束错误。这种细节第一次写的时候很容易忽略，报错信息又不太直观，排查起来挺费劲。

## 第三步——IDT 类：数据驱动的中断向量配置

GDT 配好后，接下来是 IDT。IDT 类的设计有一个和 mini kernel 版本显著不同的地方——我们用"路由表 + 循环"来替代手工逐个调用 `set_handler` 的重复代码。

先看类定义：

```cpp
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
```

异常向量、门类型、特权级都用了 scoped enum，和 GDT 的设计思路一致——类型安全、自文档化、防止误用。`IDTPrivilege::User` 的值是 `0x60`，展开成二进制就是 `0110 0000`——对应 IDT 条目 type_attr 字节中的 DPL 位（bit 5-6）设为 3，加上 Present 位（bit 7）就是 `0xE0`。

`InterruptFrame` 结构体是 C handler 接收的参数，它精确描述了 ISR stub 保存在栈上的寄存器布局：

```cpp
struct [[gnu::packed]] InterruptFrame {
    uint64_t r15, r14, r13, r12;
    uint64_t r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rdx;
    uint64_t rcx, rbx, rax;
    uint64_t error_code;
    uint64_t rip, cs, rflags, rsp, ss;
};
```

字段顺序必须和 `interrupts.S` 中的 push 顺序严格对应。汇编里先 push rax、最后 push r15，但由于栈从高地址向低地址增长，结构体的第一个字段 r15 在最低地址（即 RSP 指向的位置），最后一个字段 ss 在最高地址。error_code 之后是 CPU 自动压入的五个值（RIP、CS、RFLAGS、RSP、SS），这些不是我们 push 的，而是硬件行为。如果你在调试的时候发现打印出来的 RIP 值完全不合理——比如说显示的地址根本不在代码段范围内——那大概率是 InterruptFrame 的字段顺序和汇编的 push 顺序对不上，整个结构体错位了。这个坑真的很难发现，因为编译器不会给你任何警告。

现在看 `init()` 的核心——数据驱动的路由表：

```cpp
void IDT::init() {
    for (auto& entry : entries_) {
        entry = Entry{};
    }

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
        {ExceptionVector::NM,  isr_nm_stub, IDTPrivilege::Kernel, IDTGateType::Interrupt},
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
```

这段代码的精髓在于路由表。如果你用传统的方式写，每个向量需要单独调用一次 `set_handler`，14 个向量就是 14 行几乎一样的函数调用，复制粘贴的时候极其容易漏改某个参数。路由表把"配哪个向量、用哪个 stub、什么特权级、什么门类型"这些决策集中到一张表里，一眼就能看清楚整个中断策略。新增向量只需要在表里加一行，不用修改任何其他代码。

仔细看路由表你会发现两个特殊的设计决策。第一，#BP（向量 3）的特权级是 `User`——这是唯一一个允许从 ring 3 触发的异常，因为 `int $3` 是用户态调试器设置断点的标准方式，如果 DPL=0，用户态执行 `int $3` 会触发 General Protection Fault 而不是 #BP。第二，#BP 和 #DB（向量 1）的门类型是 `Trap` 而不是 `Interrupt`——陷阱门不会自动清除 RFLAGS.IF，这意味着在处理调试异常期间中断仍然是开启的。对于调试场景来说这是正确的行为，因为你不会希望一个断点把整个系统的中断都关掉。

`set_handler` 函数的实现就是把 64 位地址拆成三段塞进 IDT 条目的对应字段：

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

x86_64 的 IDT 条目是 16 字节，处理程序的 64 位地址被拆成低 16 位（offset_low）、中 16 位（offset_mid）、高 32 位（offset_high）三段存放。selector 统一使用 `GDT_KERNEL_CODE`（0x08），因为所有异常处理程序都运行在内核代码段。IST（Interrupt Stack Table）偏移暂时设为 0，表示不使用 IST 自动栈切换——完整的 IST 配置留到后面处理 double fault 和 NMI 的时候再做。

最后 `load()` 函数就一行汇编——`lidt` 把 IDTR 加载到 CPU：

```cpp
void IDT::load() {
    __asm__ volatile("lidt %[idtr]\n\t"
                     : : [idtr] "m"(idtr_) : "memory");
}
```

和 GDT 的 `lgdt` 不同，`lidt` 之后不需要刷新任何寄存器。IDTR 加载后立即生效，下一个异常触发时 CPU 就会使用新的 IDT。

## 第四步——ISR Stub 汇编：两个宏搞定 15 个向量

ISR stub 是中断链路中汇编密度最高的部分，也是最容易出 bug 的环节。我们在 `kernel/arch/x86_64/interrupts.S` 中定义了两个宏来生成 stub：

```asm
.macro ISR_NOERRCODE name handler
.global \name
.type \name, @function
\name:
    pushq $0              # push dummy error code 0
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

    movq %rsp, %rdi       # pass InterruptFrame* as first argument
    call \handler

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

    addq $8, %rsp         # skip error code
    iretq                  # interrupt return
.endm
```

`ISR_NOERRCODE` 处理不产生硬件错误码的异常——包括 #DE、#DB、NMI、#BP、#OF、#BR、#UD、#NM 共八个。关键的第一步是 `pushq $0`——压入一个伪错误码 0。这看似多余但对 C 代码至关重要。InterruptFrame 结构体有一个 `error_code` 字段，位于 RAX 之后、RIP 之前。如果没有这个伪错误码，结构体中的 error_code 字段实际上读到的就是 CPU 压入的 RIP 值，后面的所有字段全部错位。你可能会好奇"那有错误码的异常怎么办"——CPU 已经帮你 push 了错误码，所以 `ISR_ERRCODE` 版本不需要额外的 push 操作。

寄存器的 push 顺序也值得说一下。汇编里先 push rax、最后 push r15，但 InterruptFrame 结构体的第一个字段是 r15。这是因为栈是从高地址往低地址增长的——先 push 的在栈底（高地址），后 push 的在栈顶（低地址，即 RSP 指向的位置）。结构体的第一个字段在最低地址，所以最后 push 的 r15 刚好对应结构体的第一个字段。如果你想验证自己理解了，可以在 QEMU 里触发异常后用 GDB 看 RSP 指向的内存，和 InterruptFrame 的字段一一对比。

保存完寄存器后，`movq %rsp, %rdi` 把当前栈指针传给 C handler 作为第一个参数。按照 x86_64 System V 调用约定，第一个参数放在 RDI 里。此时 RSP 指向的就是 InterruptFrame 的起始地址——也就是 r15 字段的位置。C 代码可以直接用 `frame->rip` 这样的语法读到异常发生时的指令地址，因为结构体的字段偏移恰好对应栈上各寄存器的位置。

处理完成后，逆序 pop 恢复所有寄存器，`addq $8, %rsp` 跳过错误码（无论是伪的还是 CPU 压入的），最后 `iretq` 从栈上弹出 RIP、CS、RFLAGS、RSP、SS，恢复被中断的代码。

`ISR_ERRCODE` 宏的结构几乎一样，唯一的区别是开头没有 `pushq $0`——CPU 在触发异常时已经自动压入了错误码。受影响的六个异常是 #DF（向量 8）、#TS（向量 10）、#NP（向量 11）、#SS（向量 12）、#GP（向量 13）、#PF（向量 14）。

最后用这两个宏实例化 15 个 stub：

```asm
ISR_NOERRCODE isr_de_stub,  handle_de
ISR_NOERRCODE isr_db_stub,  handle_db
ISR_NOERRCODE isr_nmi_stub, handle_nmi
ISR_NOERRCODE isr_bp_stub,  handle_bp
ISR_NOERRCODE isr_of_stub,  handle_of
ISR_NOERRCODE isr_br_stub,  handle_br
ISR_NOERRCODE isr_ud_stub,  handle_ud
ISR_NOERRCODE isr_nm_stub, handle_nm

ISR_ERRCODE   isr_df_stub,  handle_df
ISR_ERRCODE   isr_ts_stub,  handle_ts
ISR_ERRCODE   isr_np_stub,  handle_np
ISR_ERRCODE   isr_ss_stub,  handle_ss
ISR_ERRCODE   isr_gp_stub,  handle_gp
ISR_ERRCODE   isr_pf_stub,  handle_pf
```

宏的好处在这里体现得很明显——15 个 stub 只需要 15 行声明，如果手写的话就是 15 段几乎一样的汇编代码，总量超过 500 行。更重要的是，如果以后要修改 stub 的行为（比如加 IST 支持、加嵌套中断计数），只需要改宏定义，15 个 stub 自动全部更新。

## 第五步——C 处理函数：致命与非致命的分治

ISR stub 把控制权交给 C handler 后，处理逻辑就进入我们熟悉的 C++ 世界了。`exception_handlers.cpp` 中定义了 15 个 handler 函数，核心策略是：#BP 和 #DB 是非致命异常，打印信息后通过 IRETQ 继续执行；其余全部是致命异常，打印寄存器 dump 后 `cli; hlt` 永久停机。

先看公共的寄存器 dump 函数：

```cpp
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
    kprintf("  RAX=%p  RBX=%p\n", ...);
    // ... 其余寄存器
    kprintf("  ERROR CODE = %p\n",
            reinterpret_cast<void*>(frame->error_code));
    kprintf("========================================\n");
}
```

这个函数把 InterruptFrame 里的所有字段格式化输出到串口。每个字段都用 `%p` 格式打印——在 64 位系统上 `%p` 输出完整的 16 位十六进制地址，这对于调试来说信息量最充足。CS 和 SS 用 `0x%04x` 打印成 4 位十六进制，因为段选择子永远是 16 位的。

非致命异常的处理以 #BP 为代表：

```cpp
void handle_bp(InterruptFrame* frame) {
    dump_registers(frame, "#BP", 3);
    kprintf("[EXCEPTION] Breakpoint at RIP=%p\n",
            reinterpret_cast<void*>(frame->rip));
    kprintf("[EXCEPTION] Continuing...\n");
}
```

打印完寄存器信息后直接返回——ISR stub 里的 `iretq` 会恢复到触发断点的位置继续执行。对于 `int $3` 指令来说，CPU 压入的 RIP 指向 `int $3` 的下一条指令（因为 #BP 是 trap 类型的异常，CPU 保存的是"下一条"指令的地址而不是"当前"指令），所以 IRETQ 后程序会从 `int $3` 之后继续，不会无限循环触发断点。

致命异常的处理以 #PF 为代表，它是最复杂的一个：

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
```

#PF handler 额外读取了 CR2 寄存器——CPU 在触发页错误时自动把导致缺页的线性地址写入 CR2，这是调试缺页问题时最重要的信息。错误码被解码成五个可读字段：页不存在 vs 权限冲突、读 vs 写、内核 vs 用户、保留位是否损坏、是否是指令取址触发。当你后续开发页表映射和内存管理的时候，这些信息会帮你快速定位问题根源。

所有致命 handler 最后都调用 `fatal_halt()`：

```cpp
[[noreturn]] void fatal_halt() {
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}
```

`cli` 关中断、`hlt` 停机，循环防止被 NMI 唤醒后继续执行。`[[noreturn]]` 属性告诉编译器这个函数永远不会返回，消除警告。这里千万别只用一个 `hlt` 不加循环——NMI（不可屏蔽中断）可以唤醒 `hlt` 状态的 CPU，如果不循环就会从 halt 之后继续往下执行未初始化的内存，结果不可预测。

## 第六步——kernel_main 里串起来，点火！

所有组件就绪后，在 `kernel/main.cpp` 里把整条链路串起来：

```cpp
extern "C" void kernel_main() {
    cinux::lib::kprintf_init();

    cinux::lib::kprintf("[BIG] Big kernel running @ 0x1000000\n");

    cinux::arch::g_gdt.init();
    cinux::lib::kprintf("[BIG] GDT loaded.\n");

    cinux::arch::g_idt.init();
    cinux::lib::kprintf("[BIG] IDT loaded.\n");

    cinux::lib::kprintf("[BIG] Triggering int $3 breakpoint...\n");
    __asm__ volatile("int $3");
    cinux::lib::kprintf("[BIG] Breakpoint returned, continuing.\n");

    while (1) {
        __asm__ volatile("cli; hlt");
    }
}
```

初始化顺序有讲究：`kprintf_init()` 最先，因为后续所有步骤都需要串口输出日志；然后是 GDT，因为 IDT 条目引用 GDT 中的代码段选择子；然后是 IDT；最后才触发 `int $3`。如果你把 IDT 放在 GDT 之前初始化，IDT 条目里的 selector（0x08）在 GDT 还没配好时就指向一个无效的描述符，中断一触发就 triple fault。

另一个值得注意的地方是：我们始终没有调用 `sti`（开中断）。当前我们只配了 CPU 异常向量（0-14），IRQ（32-47）的 handler 全是空的——IDT 被清零后，未配置的向量 Present 位为 0，CPU 触发未配置的向量会产生 General Protection Fault。QEMU 的 PIT 定时器中断挂在 IRQ0（向量 32），如果我们开了 `sti`，PIT 中断一触发就会连环 triple fault。所以这里千万别手滑加 `sti`——要等后面配好 PIC 和 IRQ handler 之后才能开中断。

## 上板验证

构建运行后，串口输出应该是这样的：

```
[BIG] Big kernel running @ 0x1000000
[BIG] GDT loaded.
[BIG] IDT loaded.
[BIG] Triggering int $3 breakpoint...

==== EXCEPTION: #BP (vector 3) ====
  RIP   = 0xffffffff80XXXXXX   CS  = 0x0008
  RFLAGS= 0x00000000000000XX
  RSP   = 0xffffffff80XXXXXX   SS  = 0x0010
  RAX=0x0000000000000000  RBX=0x000000000000XXXX
  RCX=0x0000000000000000  RDX=0x0000000000000000
  RSI=0x0000000000000000  RDI=0x000000000000XXXX
  RBP=0x0000000000000000  R8 =0x0000000000000000
  R9 =0x0000000000000000  R10=0x0000000000000000
  R11=0x0000000000000000  R12=0x0000000000000000
  R13=0x0000000000000000  R14=0x0000000000000000
  R15=0x0000000000000000
  ERROR CODE = 0x0000000000000000
========================================
[EXCEPTION] Breakpoint at RIP=0xffffffff80XXXXXX
[EXCEPTION] Continuing...
[BIG] Breakpoint returned, continuing.
```

看到 `[BIG] Breakpoint returned, continuing.` 这行输出，就说明整个链路完全通畅：`int $3` 触发 → CPU 查 IDT 向量 3 → 跳到 `isr_bp_stub` → 保存寄存器 → 调用 `handle_bp` → 打印 dump → 返回 → ISR stub 恢复寄存器 → IRETQ → `kernel_main` 继续执行 `int $3` 之后的代码。

CS = 0x0008 确认了内核代码段选择子正确加载——0x08 对应 GDT 索引 1（0x08 >> 3 = 1），就是我们的 kernel code segment。SS = 0x0010 对应 GDT 索引 2，是 kernel data segment。ERROR CODE = 0 证实了 ISR_NOERRCODE 宏正确地压入了伪错误码。RIP 指向 `int $3` 之后那条指令的地址——因为 #BP 是 trap 类异常，CPU 压入的 RIP 指向"导致异常的指令"的下一条。

## 设计决策回顾：为什么用 class 而不是 C 风格

你可能会问——mini kernel 里那套 `gdt_init()` / `idt_init()` 的 C 风格全局函数不是挺好的吗？为什么要花力气封装成 class？

这个问题的答案不在于"当前 milestone 能不能用"，而在于"后续扩展好不好维护"。mini kernel 是一个生命周期有限的验证平台，它做到 milestone 009 就基本完成任务了，代码量不会再大幅增长。但大内核是一个持续演化的项目，GDT 和 IDT 的管理逻辑会越来越复杂——后续要加 APIC 支持（需要重配 IDT 的 IRQ 向量）、用户态进程（需要完善 TSS 的 RSP0/IST 字段）、SMP 多核（每个核心需要独立的 GDT/IDT/TSS 实例）、信号处理（需要修改 IDT 的用户态门）。

用 class 封装后，GDT 和 IDT 的状态（entries 数组、GDTR/IDTR 指针、TSS 实例）被绑在对象上，而不是散落在全局命名空间里。未来支持 SMP 时，每个核心只需要实例化自己的 `GDT` 和 `IDT` 对象，互不干扰。如果是 C 风格的全局数组加全局函数，多核场景下的状态管理会变得非常混乱——你需要用 per-CPU 变量或者数组索引来区分不同核心的描述符表，代码可读性和可维护性都会大幅下降。

TSS 占位的设计也是同理。当前 milestone 我们把 TSS 填进 GDT、用 `ltr` 加载了 TR，但 TSS 结构体里所有字段都是零。这看起来像是"做了一半的事情"，但它是刻意的——先把框架搭好，确保 TSS 描述符格式正确、`ltr` 不报错，后续再往里面填具体的栈切换值。如果你等需要 TSS 的时候才加 GDT 的 TSS 支持，那时候需要同时调试 TSS 内容和 GDT 格式两个维度的问题，出错概率更高。

数据驱动的路由表也是一个值得坚持的模式。每次新增中断向量——不管是后续加硬件 IRQ、系统调用门、还是 APIC 中断——只需要在路由表里加一行，不用修改 `init()` 的核心逻辑。这种"配置和逻辑分离"的设计在内核开发中非常实用，因为中断配置的变更频率远高于加载逻辑的变更频率。

## 收尾

到这里，大内核的 GDT/IDT/中断系统就完整搭好了。回顾一下我们做了什么：用 C++ class 封装了 GDT（7 个描述符，含 TSS）和 IDT（15 个 CPU 异常向量），用 scoped enum 定义了类型安全的描述符标志位，用数据驱动的路由表替代了重复的 set_handler 调用，用两个汇编宏生成了 15 个 ISR stub，用致命/非致命分治策略处理了所有已知 CPU 异常，最后在 `kernel_main` 里用 `int $3` 触发断点验证了整条链路。

下一步的方向很明确：首先是 PIC（8259A 可编程中断控制器）的初始化和 IRQ 重映射，把硬件中断的向量号从默认的 0x08-0x0F 重映射到 0x20-0x2F，避免和 CPU 异常向量冲突。然后是键盘驱动（IRQ1）和定时器驱动（IRQ0），这两个是操作系统"能和外部世界交互"的最基础设施。配好 IRQ 之后才能 `sti` 开中断，内核才算真正"活"起来。

完结撒花——大内核终于能优雅地处理异常了，不再是那个"一句话不说就 triple fault"的莽夫了。
