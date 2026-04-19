# 010 Big Kernel GDT/IDT/中断系统 - 通读版

**本章 git tag**：`010_big_kernel_gdt_idt`，上一章 tag：`009_large_kernel_entry`

---

## 本章概览

到了 milestone 009，我们的大内核已经可以跑起来了——串口能输出、kprintf 能打字、ELF 加载器稳如老狗。但说实话，一个没有中断处理能力的内核就像一辆没有刹车的车，只要 CPU 遇到一点点意外情况——除零、缺页、非法指令——整个系统就直接 triple fault 然后 QEMU 重启，你连发生了什么都不知道。这一章我们要做的事情很明确：给大内核装上 GDT 和 IDT，让 CPU 异常不再是一脚踩空就完蛋的黑洞，而是能被捕获、被报告、被诊断的"可控事故"。

本章的核心产出是四个模块：GDT（全局描述符表）封装了段描述符的构建和加载逻辑，为内核提供代码段、数据段、用户段和 TSS 的完整段管理；IDT（中断描述符表）用数据驱动的路由表把 14 个 CPU 异常向量注册到对应的 ISR 汇编桩和 C 处理函数；ISR 汇编宏（`ISR_NOERRCODE` / `ISR_ERRCODE`）负责在异常触发时保存完整寄存器现场、构建 InterruptFrame 结构体、调用 C handler；异常处理函数则负责把寄存器快照通过串口 dump 出来，致命异常就永久停机，非致命异常（比如断点）就优雅地恢复执行。milestone 的验收标准非常直观：执行 `int $3` 触发一个软件断点，串口打印完整的寄存器 dump，然后内核继续运行，不死机。

关键设计决策方面，我们选了混合 Gate 策略——`#BP` 和 `#DB` 用 Trap Gate（不清 IF 标志），其余异常一律用 Interrupt Gate（清 IF 标志），这样做的原因是调试异常需要在中断处理后保持中断使能状态的语义；GDT 和 IDT 都用 class 封装、scoped enum 作为 API 类型，让编译器帮我们拦截类型错误；IDT 初始化采用 data-driven 的 Route 配置表加循环注册，替代了 14 个重复的 `set_handler` 调用；`extern "C" {}` 块语法被用来统一声明汇编桩和 C handler 的链接符号。和 xv6 对比的话，xv6 的 IDT 设置更简洁粗暴——它用一个大数组加宏直接展开，没有 class 封装也没有 enum 类型。Linux 早期版本的 IDT 注册则更接近我们的做法：用一张表描述向量到 handler 的映射关系，然后循环填充 IDT entry。我们的设计比 xv6 多了一层类型安全，比 Linux 少了一层宏魔法，算是在可读性和工程性之间取了个平衡。

---

## 架构图

```
GDT / IDT / 中断处理全链路：

  CPU 异常触发（如 int $3 除零 等）
       │
       │  CPU 自动行为：
       │    1. 根据 IDT[vector].type_attr 决定 Gate 类型
       │    2. 保存 RFLAGS / RIP / CS / RSP / SS 到内核栈
       │    3. 若 Interrupt Gate → 清 IF（禁止中断嵌套）
       │    4. 跳转到 IDT[vector].offset 指向的 ISR stub
       ▼
  ┌─ interrupts.S: ISR_NOERRCODE / ISR_ERRCODE 宏 ──────────┐
  │                                                            │
  │  对于无硬件错误码的异常：push $0（dummy error code）          │
  │  对于有硬件错误码的异常：CPU 已 push，跳过此步               │
  │  保存 15 个通用寄存器 → 栈上形成 InterruptFrame             │
  │  movq %rsp, %rdi → 传参给 C handler                        │
  │  call handle_xx → 跳转到 C 处理函数                        │
  │  恢复 15 个通用寄存器                                       │
  │  addq $8, %rsp → 弹出 error code                          │
  │  iretq → 恢复 RIP/CS/RFLAGS/RSP/SS                        │
  └────────────────────────────────────────────────────────────┘
       │
       │  InterruptFrame* 作为唯一参数
       ▼
  ┌─ exception_handlers.cpp ──────────────────────────────────┐
  │                                                            │
  │  dump_registers(frame, name, vector)                       │
  │    → kprintf 输出 RIP/RFLAGS/RSP/RAX...等全部寄存器         │
  │                                                            │
  │  非致命异常（#BP, #DB）:                                    │
  │    → 打印信息后返回 → ISR stub iretq → 继续执行             │
  │                                                            │
  │  致命异常（其余所有）:                                       │
  │    → 打印寄存器 dump                                       │
  │    → fatal_halt() → cli; hlt 死循环                        │
  │                                                            │
  │  #PF 特殊处理:                                             │
  │    → 读取 CR2 获取缺页地址                                  │
  │    → 解析 error code 位域（present/write/user/fetch）       │
  └────────────────────────────────────────────────────────────┘


  初始化调用链：

  kernel_main()
      │
      ├── g_gdt.init()                         ← GDT 必须先于 IDT
      │     │
      │     ├── 填充 entries_[0..6]
      │     │     [0] Null
      │     │     [1] Kernel Code (0x08)  Ring0, Execute/Read, Long Mode
      │     │     [2] Kernel Data (0x10)  Ring0, Read/Write, 32-bit
      │     │     [3] User Code   (0x1B)  Ring3, Execute/Read, Long Mode
      │     │     [4] User Data   (0x23)  Ring3, Read/Write, 32-bit
      │     │     [5] TSS Low     (0x28)  104 bytes
      │     │     [6] TSS High           (高32位地址续接)
      │     │
      │     ├── lgdt (加载 GDTR)
      │     ├── 远跳转刷新 CS（lretq）
      │     ├── 刷新 DS/ES/FS/GS/SS
      │     └── ltr 加载 TSS 选择子
      │
      ├── g_idt.init()                         ← IDT 依赖 GDT 的段选择子
      │     │
      │     ├── 清零 entries_[0..255]
      │     │
      │     ├── 遍历 Route 配置表，逐项注册：
      │     │     {vector, stub, privilege, gate_type}
      │     │     → set_handler() 拆分地址到 IDT entry 的 3 段 offset
      │     │
      │     └── lidt 加载 IDTR
      │
      └── __asm__ volatile("int $3")           ← 触发断点测试


  栈帧布局（异常发生后的内核栈）：

  高地址
  ┌─────────────┐
  │     SS      │  ← CPU 自动 push
  │     RSP     │  ← CPU 自动 push
  │    RFLAGS   │  ← CPU 自动 push
  │     CS      │  ← CPU 自动 push
  │     RIP     │  ← CPU 自动 push
  │  Error Code │  ← CPU push（有错误码的异常）或 ISR stub push $0
  ├─────────────┤
  │     RAX     │  ← ISR stub push
  │     RBX     │
  │     RCX     │
  │     RDX     │
  │     RBP     │
  │     RSI     │
  │     RDI     │
  │     R8      │
  │     R9      │
  │     R10     │
  │     R11     │
  │     R12     │
  │     R13     │
  │     R14     │
  │     R15     │  ← ISR stub push（最后 push，%rsp 指向此处）
  └─────────────┘
  低地址 → %rsp 传给 C handler 作为 InterruptFrame*
```

---

## 关键代码精讲

### GDT：从裸数组到类型安全的 class

上一章我们的大内核刚能跑起来的时候，实际上还跑在 mini kernel 的 Bootloader 建立的那个简陋 GDT 上——只有三个 entry（null、code、data），够用但不够看。现在我们要建立一个完整的 GDT，包含内核代码段、内核数据段、用户代码段、用户数据段和 TSS，为大内核后续的所有特权级操作和中断处理打好基础。

我们来看 GDT 的 class 设计。整个思路是把 GDT entry 的构造细节藏在 class 内部，对外只暴露语义清晰的 enum 和 `init()` 方法。`gdt.hpp` 首先定义了段选择子常量——这些值看起来像是随意选的魔术数字，但实际上它们是严格按照 GDT entry 索引左移 3 位计算出来的。比如 `GDT_KERNEL_CODE = 0x08` 意味着它是 GDT 的第 1 个 entry（index=1），`0x08 = 1 << 3`，低 3 位是 TI（Table Indicator）和 RPL（Requestor Privilege Level），这里都是 0。同理 `GDT_KERNEL_DATA = 0x10` 是第 2 个 entry，`GDT_USER_CODE = 0x1B` 是第 3 个 entry 加上 RPL=3。

段描述符的 Access Byte 和 Flags 被建模成了两个 scoped enum：`SegmentAccess` 和 `SegmentFlags`。`SegmentAccess` 的每个枚举值都是 Access Byte 中对应位的掩码——`Present` 是 bit 7，`Ring0` 是 bits 5-6 为 00，`Ring3` 是 bits 5-6 为 11（即 `3 << 5 = 0x60`），`CodeData` 是 bit 4（区分代码/数据段和系统段），`Executable` 是 bit 3，`ReadWrite` 是 bit 1。这种设计让我们可以用 `operator|` 组合这些标志，得到一个完整的 Access Byte，同时编译器会在类型层面拒绝我们把一个 `SegmentFlags` 误传给需要 `SegmentAccess` 的地方。

GDT class 内部最值得说的是三个 constexpr factory 函数。`null_entry()` 返回全零的 null descriptor——这是 x86 架构的硬性要求，GDT 的第 0 项必须为空。`segment_entry()` 接受 `SegmentAccess` 和 `SegmentFlags` 两个参数，把 limit 填满（`0xFFFF`），base 全零（我们在 long mode 下不需要分段寻址），然后把 flags 移到 `flags_limit_high` 字节的高 4 位、limit 的高 4 位放在低 4 位。这个字段的布局总是让新手困惑——一个 8 字节的 GDT entry，第 6 个字节（从 0 计数）的高 nibble 是 granularity/longmode/size 标志，低 nibble 是 limit 的 bits 16-19。我们的代码里把它表达为 `(flags << 4) | 0x0F`，其中 `0x0F` 就是 limit 高 4 位的全 1 填充。

TSS 的处理稍微特殊一点。在 64-bit 模式下，TSS descriptor 占两个 GDT slot（16 字节而不是 8 字节），因为需要容纳 64 位基地址。`tss_low_entry()` 填写前 8 个字节，包含 limit、base 的低 32 位和 access byte（`Present | TSS64Avail = 0x89`）；`tss_high_entry()` 填写后 8 个字节，包含 base 的高 32 位，其余字段为零。这里有一个容易踩坑的地方——TSS 的 access byte 是 `0x89`，这个值来自 Intel SDM Vol. 3A Table 3-2，它表示"64-bit TSS Available"。如果你不小心用了 `0x81`（32-bit TSS Available），CPU 会在 long mode 下拒绝加载它。

`init()` 方法的流程很直接：先填 null entry，然后依次填内核代码段（Ring0、Executable、LongMode）、内核数据段（Ring0、ReadWrite、Size32——注意这里没有设 LongMode，因为数据段在 long mode 下这个位必须为 0）、用户代码段（Ring3 + Executable + LongMode）、用户数据段（Ring3 + ReadWrite），最后处理 TSS。TSS 的地址通过 `reinterpret_cast<uint64_t>(&tss_)` 获取，因为 TSS 是 class 的成员变量，地址在运行时才确定。填充完所有 entry 后，设置 GDTR 的 limit 和 base，然后调用 `load()`。

`load()` 函数是一段内联汇编，做的事情非常关键但也非常容易出错。首先是 `lgdt` 指令加载 GDTR，但这还不够——x86 的段寄存器在 GDTR 被更新后不会自动刷新，CS 寄存器尤其不会。所以我们必须用一个远跳转（far return）来强制刷新 CS：先把新的 CS 选择子 `push` 到栈上，再把 `1f` 标签的地址 push 上去，然后 `lretq`——这相当于一个 "far return"，CPU 会从栈上弹出新的 RIP 和 CS，从而加载新的代码段选择子。之后用 `mov` 把数据段选择子加载到 DS/ES/FS/GS/SS。最后用 `ltr` 指令加载 TSS 选择子到 TR 寄存器。

这里有一个很容易被忽视的细节：我们的 TSS 是一个全零的占位符（104 字节，全部为零）。一个全零的 TSS 意味着没有 IST（Interrupt Stack Table）条目，没有 privilege-level 切换时的 RSP 指针，也没有 I/O Permission Bitmap。对于当前 milestone 来说这完全够用——我们只在 Ring 0 运行，中断栈就是当前内核栈，不需要 IST 也不需要特权级切换。但到了 milestone 022（完整 TSS）的时候，这个占位符会被填上真正有意义的内容。

### IDT：数据驱动的路由表设计

GDT 搭好了舞台，IDT 才是真正让中断处理成为可能的主角。IDT 的工作原理很简单——它是一个最多 256 项的数组，CPU 在收到中断/异常时，用向量号作为索引查表，找到对应的 handler 地址然后跳过去。但在 x86_64 下，IDT entry 的结构比 32 位模式复杂了不少——每个 entry 是 16 字节，handler 地址被拆成三段（low 16 bits、mid 16 bits、high 32 bits）分散存放，这是因为一个 entry 除了地址还要放段选择子、IST 偏移、type/attribute 字节和保留字段。

我们来看 `idt.hpp` 的设计。三个 scoped enum 承担了类型安全的职责：`ExceptionVector` 枚举了 0-14 号 CPU 异常向量，`IDTGateType` 区分了 Interrupt Gate（`0x0E`，会清 IF 标志禁止中断嵌套）和 Trap Gate（`0x0F`，保持 IF 不变），`IDTPrivilege` 区分了 Ring 0 和 Ring 3 的 DPL。`make_idt_attr()` 是一个 constexpr 函数，它把 Present 位（`0x80`）、DPL 和 Gate Type 组合成一个字节的 type_attr 值。这种分离关注点的设计让 IDT entry 的配置变成了一种声明式的数据描述——你只需要说"这个向量用什么 stub、什么特权级、什么 gate type"，不需要操心 type_attr 字节的位运算。

`InterruptFrame` 结构体是我们 ISR 汇编宏和 C handler 之间的契约。它的字段顺序严格对应栈上的布局——从低地址到高地址依次是 R15（最后 push 的，所以最低）、R14、R13...一直到 RAX，然后是 error_code、RIP、CS、RFLAGS、RSP、SS（CPU 自动 push 的）。当 ISR stub 把 `%rsp` 作为第一个参数传给 C handler 时，`%rsp` 指向的就是 R15 的地址，而 `InterruptFrame*` 指针正好指向这个结构体的开头。这种"栈帧即结构体"的技巧在 OS 开发中非常常见，它让 C 代码可以用自然的方式访问所有保存的寄存器，而不需要手写偏移量。

接下来看 `idt.cpp` 的实现。文件开头有两段 `extern "C" {}` 块——第一段声明了所有 ISR stub 函数（定义在 `interrupts.S`），第二段声明了所有 C handler 函数（定义在 `exception_handlers.cpp`）。这两个 `extern "C"` 块的目的是告诉 C++ 编译器这些符号使用 C 语言的链接规范（name mangling 关闭），这样汇编代码和 C++ 代码才能通过符号名互相引用。这里用了一个很实用的 C++ 语法——`extern "C" {}` 可以包裹多个声明，比在每个函数前面单独写 `extern "C"` 要简洁得多，可读性也更好。不过要注意，`extern "C"` 只影响链接规范，不影响类型系统——所以 `handle_de(InterruptFrame*)` 的参数类型仍然是 C++ 的 `InterruptFrame*`，命名空间查找也仍然遵循 C++ 规则。

`IDT::init()` 的核心是那张 Route 配置表。它是一个局部结构体数组，每个元素包含四个字段：异常向量号、ISR stub 函数指针、特权级和 Gate 类型。14 个异常被分成两类——`#BP`（向量 3）和 `#DB`（向量 1）用 Trap Gate，其余全部用 Interrupt Gate。这个区分的依据是 Intel 手册的规定：断点和调试异常是"陷阱"（trap）语义，它们在指令执行后才报告，而且不应该改变中断使能状态；而其他异常（如除零、缺页、一般保护错误）是"故障"（fault）或"中止"（abort），它们的处理过程中如果允许中断嵌套会带来不可预测的行为。一个额外的细节是 `#BP` 的特权级被设为 `User`（DPL=3），这是因为 `int $3` 指令（触发 `#BP`）是可以在用户态执行的，如果 DPL 设为 0，用户态执行 `int $3` 就会触发 `#GP`（General Protection Fault）而不是 `#BP`。

注册循环遍历 Route 表，对每一项调用 `set_handler()`。`set_handler()` 的实现是把 64 位 handler 地址拆成三段填入 IDT entry——这是 x86_64 IDT entry 结构的怪异之处，offset 被拆成了 `offset_low`（bits 0-15）、`offset_mid`（bits 16-31）和 `offset_high`（bits 32-63）三个不连续的字段。段选择子统一使用 `GDT_KERNEL_CODE`（`0x08`），因为我们所有的异常处理代码都在内核代码段里。IST 字段暂时填 0（不使用 IST），等到 milestone 022 实现 TSS 的 IST 后才需要改。所有 entry 注册完毕后，设置 IDTR 的 limit 为 `sizeof(entries_) - 1`（256 * 16 - 1 = 4095），base 为 entries 数组的地址，然后 `lidt` 加载。

### ISR 汇编宏：构建寄存器快照

ISR stub 的任务是连接 CPU 的异常分发机制和我们的 C 处理函数。在异常触发时，CPU 已经帮我们 push 了 SS、RSP、RFLAGS、CS 和 RIP 五个值（以及部分异常的 error code），但这还不够——C 处理函数需要看到所有通用寄存器的值，而且 C 函数本身可能会修改寄存器。所以 ISR stub 必须在跳转到 C handler 之前把所有通用寄存器保存到栈上，在 handler 返回之后再恢复。

`interrupts.S` 定义了两个宏：`ISR_NOERRCODE` 和 `ISR_ERRCODE`。两者的唯一区别是前者需要 push 一个 dummy 的 error code 0（为了统一栈帧布局），而后者不需要（CPU 已经 push 了真正的 error code）。这个设计决策很重要——如果我们不统一布局，那么 InterruptFrame 结构体的 error_code 字段位置就会因异常类型而异，C handler 就得根据向量号来判断栈帧格式，这会让代码变得既丑陋又容易出错。用 dummy 0 填充之后，所有异常的栈帧格式完全一致，C handler 可以用同一个 InterruptFrame 结构体来解读。

宏的执行流程分为五个阶段。第一阶段，对于无错误码的异常，push `$0` 填位。第二阶段，依次 push 15 个通用寄存器——顺序是 RAX、RBX、RCX、RDX、RBP、RSI、RDI、R8-R15。这个顺序不是随意的，它和 InterruptFrame 结构体的字段顺序完全对应。注意 push 的顺序是 RAX 先 push（所以 RAX 在栈的最高位置），R15 最后 push（在栈的最低位置，也就是 `%rsp` 指向的位置），而 InterruptFrame 的第一个字段是 R15——这样 `%rsp` 指向的恰好是 `frame->r15`。

第三阶段，`movq %rsp, %rdi` 把当前栈指针传给 `%rdi`（System V AMD64 ABI 的第一个参数寄存器），然后 `call handler` 调用对应的 C 处理函数。第四阶段，handler 返回后，逆序 pop 15 个寄存器（R15 最先 pop，RAX 最后 pop）。第五阶段，`addq $8, %rsp` 跳过 error code（无论它是 dummy 还是 CPU push 的），然后 `iretq` 从栈上恢复 RIP、CS、RFLAGS、RSP 和 SS，回到异常发生前的执行点。

文件末尾用这两个宏实例化了 14 个 ISR stub，每个 stub 绑定到对应的 C handler。无错误码的异常包括 `#DE`（0）、`#DB`（1）、NMI（2）、`#BP`（3）、`#OF`（4）、`#BR`（5）、`#UD`（6）、`#NM`（7），共 8 个。有错误码的异常包括 `#DF`（8）、`#TS`（10）、`#NP`（11）、`#SS`（12）、`#GP`（13）、`#PF`（14），共 6 个。注意向量 9（Coprocessor Segment Overrun）和向量 15（保留）没有被注册——前者在现代 CPU 上已经很少触发了，后者是 Intel 保留的向量号，不应该被使用。

### 异常处理函数：从 dump 到诊断

`exception_handlers.cpp` 中的所有 handler 函数都被包裹在 `extern "C" {}` 块里，因为它们要被 `interrupts.S` 中的 `call` 指令引用，而 `call` 使用的是 C 链接的符号名。文件内部有一个匿名 namespace 包含两个辅助函数——`dump_registers()` 负责把 InterruptFrame 里的所有寄存器值格式化输出到串口，`fatal_halt()` 是一个永不返回的死循环（`cli; hlt`）。

`dump_registers()` 的输出格式经过精心设计：先是异常名和向量号作为标题行，然后是控制流相关的寄存器（RIP、CS、RFLAGS、RSP、SS），最后是所有通用寄存器（成对排列，方便阅读），以及 error code。每个 handler 在调用 `dump_registers()` 之后根据异常的严重程度做不同处理。

这里的设计体现了一种务实的策略：把所有异常分成两类。非致命异常只有两个——`#BP`（断点）和 `#DB`（调试），它们在打印诊断信息后直接返回，ISR stub 的 `iretq` 会让 CPU 回到异常发生点继续执行。所有其他异常都被视为致命的，handler 在打印诊断信息后调用 `fatal_halt()` 永久停机。这种二元分类在当前阶段完全够用——我们没有实现真正的异常恢复机制（比如 `#PF` 的按需分页、`#GP` 的信号投递），所以除了断点和调试之外，任何异常都意味着系统已经进入不可恢复的状态。

`#PF`（Page Fault）的 handler 值得单独说说——它是所有异常 handler 中信息最丰富的一个。它首先通过 `movq %%cr2, %0` 读取 CR2 寄存器，CPU 在发生缺页时会自动把触发缺页的线性地址写入 CR2。然后它解析 error code 的各个位域：bit 0 区分"页不存在"和"权限违反"，bit 1 区分"读"和"写"，bit 2 区分"内核态"和"用户态"，bit 3 检查保留位违规，bit 4 区分"数据访问"和"指令取指"。这些信息被组合成一条可读的英文消息输出到串口，对于缺页调试来说极其有用。

### kernel_main：点火测试

`kernel_main()` 中的初始化顺序非常有讲究——GDT 必须在 IDT 之前初始化。原因是 IDT entry 中包含了段选择子（指向 GDT 中的代码段），如果 GDT 还没有加载就注册 IDT entry，那些段选择子就指向了错误的描述符。先加载 GDT、刷新段寄存器，再加载 IDT——这个顺序是 x86 架构的硬性依赖。

测试代码只有一行 `__asm__ volatile("int $3")`，但它验证了整个中断处理链路：CPU 收到向量 3 → 查 IDT[3] → 找到 `isr_bp_stub` → stub push dummy error code 和 15 个寄存器 → 调用 `handle_bp()` → handler 调用 `dump_registers()` 打印寄存器快照 → handler 返回 → stub 恢复寄存器并 `iretq` → 回到 `kernel_main` 继续执行 `kprintf("[BIG] Breakpoint returned, continuing.")`。如果链路中任何一环出问题——GDT 没加载、IDT entry 没填对、ISR stub 的 push/pop 不匹配、`iretq` 的栈不平衡——都会导致 triple fault 和 QEMU 重启。

还有一个需要注意的点：`kernel_main` 的注释里明确写了"do NOT enable interrupts (`sti`) yet"。这是因为我们虽然注册了 CPU 异常的 handler，但还没有注册硬件中断（IRQ 0-15、或者 APIC 的中断向量）的 handler。如果现在就 `sti`，PIT 定时器（IRQ 0）很快就会触发一个中断，CPU 查 IDT 找不到对应的 handler，就会触发 `#GP` 或者直接 triple fault。

---

## 设计决策深度分析

### 决策一：混合 Gate 策略——Trap Gate 和 Interrupt Gate 的取舍

**问题**：IDT entry 的 Gate 类型决定 CPU 在跳转到 handler 之前是否清 IF（中断标志），这直接影响到异常处理过程中是否允许嵌套中断。对于 14 个 CPU 异常向量，每个都用什么 Gate 类型是一个需要认真考虑的设计选择。

**本项目的做法**：我们用了混合策略——`#BP`（向量 3）和 `#DB`（向量 1）用 Trap Gate（type = 0x0F，不清 IF），其余所有异常用 Interrupt Gate（type = 0x0E，清 IF）。这个策略的依据是 Intel SDM Vol. 3A Section 6.12.1 的推荐——调试异常和断点在语义上是"陷阱"，它们报告的是指令执行后（或执行中，但不影响程序继续）的状态，保持中断使能状态更符合它们的语义。而其他异常（除零、缺页、保护错误等）在处理期间如果被中断嵌套，可能导致栈溢出或状态不一致。

**备选方案**：最简单的做法是所有异常都用 Interrupt Gate。很多 OS 教程项目（包括早期的 xv6）就是这样做的一刀切——反正异常处理很快就会结束，清 IF 不会有明显影响。另一个极端是全部用 Trap Gate，允许所有异常处理过程中被中断——这在理论上能减少中断延迟，但实践中风险太大。

**为什么不选备选方案**：全用 Interrupt Gate 在功能上没问题，但在语义上不精确——GDB 用 `int $3` 设置断点时，期望断点处理过程中中断仍然使能，这样 GDB 才能在断点命中时接收来自调试器的其他中断。全用 Trap Gate 则更危险——设想 `#PF` handler 正在处理缺页，此时又来了一个定时器中断，中断 handler 访问了一个缺页的地址，于是又触发 `#PF`，嵌套的 `#PF` 会导致栈指针被覆盖（除非用了 IST），最终 triple fault。

**如果要扩展/改进**：当引入 APIC 和硬件中断后，这个策略还需要审视——IRQ handler 应该用 Interrupt Gate（防止中断嵌套），但某些高优先级中断（如 NMI、Machine Check）需要特殊处理。此外，对于 `#PF` handler，应该使用 TSS 的 IST 机制为它分配独立的栈，这样即使发生了嵌套 `#PF`（比如常规栈本身缺页），也不会破坏 handler 的执行环境。

### 决策二：Data-driven 的 IDT 初始化 vs 手写重复调用

**问题**：我们有 14 个异常需要注册，每个注册调用有 5 个参数（向量号、stub 地址、段选择子、type_attr、IST）。如果手写 14 个 `set_handler()` 调用，代码会非常冗长且难以维护——每次增删一个异常处理都要在 `init()` 函数里找到对应的那一行。

**本项目的做法**：在 `init()` 内部定义一个局部的 `Route` 结构体和静态数组，把 14 个异常的配置信息以数据表的形式声明出来，然后用一个 `for` 循环遍历这张表并调用 `set_handler()`。这种 "data-driven" 的写法让配置信息集中在一个地方，格式统一，增删改都很方便——只需要修改表中的某一行。

**备选方案**：最直接的做法是 14 个手写调用，每个调用一行。Linux 内核的早期版本（比如 2.4）就是这样做的——一个大的 `set_intr_gate()` / `set_trap_gate()` 调用序列。另一个方案是用 X-macro 预处理技巧——把配置信息放在一个头文件里用宏列表表示，然后在 `init()` 里用 `#define` / `#undef` 展开。

**为什么不选备选方案**：14 个手写调用的问题不只是冗长——更关键的是它把"配置"和"执行"混在了一起。当你需要比较两个异常的配置差异时，你的眼睛要在 14 行代码之间来回跳，很容易漏看。X-macro 方案虽然更 DRY，但可读性更差——宏展开后的代码无法调试，IDE 也无法正确跳转。我们的 Route 表方案达到了很好的平衡——数据是纯声明式的（一眼就能看到所有配置），执行是一个简单循环（不可能出错），而且 C++ 编译器可以在编译期检查类型（scoped enum 防止拼写错误）。

**如果要扩展/改进**：当硬件中断（IRQ）和系统调用（`syscall` / `int 0x80`）需要注册时，Route 表只需要增加更多行。更进一步的话，可以把 Route 表的定义移到头文件中或者用 constexpr 数组，让其他模块（比如自测框架）能够遍历所有已注册的异常。如果未来需要支持动态注册和注销 handler（比如模块加载），则需要在 Route 表的基础上增加一个红黑树或哈希表的 handler dispatch 层。

### 决策三：`extern "C" {}` 块语法 vs 逐函数声明

**问题**：在 C++ 内核中，汇编文件定义的符号（ISR stub）和 C 链接规范的 handler 函数都需要 `extern "C"` 声明来禁用 name mangling。声明方式有两种——在每一个函数声明前加 `extern "C"`，或者用 `extern "C" {}` 块把一组声明包起来。

**本项目的做法**：在 `idt.cpp` 中用了两个 `extern "C" {}` 块——一个包 14 个 ISR stub 声明，一个包 14 个 C handler 声明。在 `exception_handlers.cpp` 中用一个 `extern "C" {}` 块包所有 handler 定义。这种做法让同一类符号的声明集中在一起，视觉上更清晰。

**备选方案**：最常见的做法是在每个需要 C 链接的函数前单独写 `extern "C"`。很多 OS 项目（包括 xv6）把所有需要 `extern "C"` 的函数声明放在一个头文件里，然后只在头文件里用一个 `extern "C" {}` 块。另一个方案是用 `extern "C"` 包含整个文件的编译（通过编译选项），但这会影响文件中所有符号。

**为什么不选备选方案**：逐函数声明在数量多的时候非常啰嗦——14 个 stub 加 14 个 handler，每个前面加 `extern "C"`，会增加 28 行噪音。全部放进头文件的方案其实和我们的做法等价，区别只是位置不同——我们选择把声明放在 `.cpp` 文件的顶部（因为它们只在那个文件内部使用），而不是暴露到头文件中（那样会污染全局命名空间）。对于 `exception_handlers.cpp` 的 handler 定义来说，`extern "C" {}` 块更是必须的——因为这些函数是定义（不是声明），它们必须被编译为 C 链接规范。

**如果要扩展/改进**：随着 handler 数量的增长（加入 IRQ、系统调用等），可以考虑在头文件中集中声明所有 `extern "C"` 符号，然后 `.cpp` 文件 include 这个头文件。或者更进一步，用宏自动生成 `extern "C"` 声明——定义一个 `CINU_EXTERN_C_HANDLER(name)` 宏，展开为 `extern "C" void name(InterruptFrame*)`，这样声明列表可以和 Route 表保持同步。

---

## 常见变体与扩展方向

**1. 实现硬件中断（IRQ）处理** ⭐⭐

当前我们只注册了 CPU 异常（向量 0-14），还没有触及硬件中断。扩展方向是在 IDT 中注册 8259A PIC 或 APIC 的中断向量（通常是 32-47 或 32-255），编写时钟中断（PIT/HPET）、键盘中断（IRQ1）的 handler。这个扩展的难点不在于 IDT 注册（加几行 Route 就行），而在于 8259A / APIC 的初始化和 EOI（End Of Interrupt）信号发送。完成这个扩展后，你就可以在内核里实现定时器、键盘输入等真正"交互式"的功能了。

**2. 为 `#PF` 实现 IST（Interrupt Stack Table）** ⭐⭐

当前 `#PF` handler 在常规内核栈上运行，如果缺页恰好发生在内核栈本身（比如栈增长越界），handler 就会在一个无效的栈上执行，导致 triple fault。解决方案是为 `#PF` 分配一个独立的 IST 栈（在 TSS 的 `ist[0]` 中设置），并在 IDT entry 的 IST 字段填入 IST 索引（1-7）。这样即使常规栈崩了，`#PF` handler 也能在 IST 栈上安全执行。

**3. 实现异常恢复而非永久停机** ⭐⭐⭐

当前所有非调试异常都是 fatal halt——这很不 Linux。真正的操作系统在遇到 `#PF` 时会尝试分配物理页并映射，遇到 `#GP` 时会向进程投递 SIGSEGV 信号。要实现这个扩展，你需要一个物理内存管理器（PMM）、一个虚拟内存管理器（VMM）和一个进程/信号系统——这是一个大工程，但也是从"教学内核"走向"可用内核"的关键一步。

**4. 用 `cli`/`sti` 实现临界区和中断计数** ⭐

当前的 Interrupt Gate 会自动清 IF，但有时候我们需要在 C 代码中手动控制中断使能。可以实现一个简单的中断计数器——`push_cli()` 时如果计数器从 0 变为 1 则 `cli`，`pop_sti()` 时如果计数器从 1 变为 0 则 `sti`。这种嵌套式的中断控制能防止在嵌套调用中过早 `sti`。

**5. 添加 `#DF`（Double Fault）的 IST 栈和 TSS 完善** ⭐⭐

Double Fault 是"异常处理中又触发了异常"的情况——最常见的就是 `#PF` handler 执行时又触发了 `#PF`。由于 Double Fault 发生时内核栈可能已经损坏，必须使用 IST 为 `#DF` 分配独立栈。这个扩展需要完善 TSS 结构（填充 `ist[1]` 和对应的栈空间），然后在 IDT entry 的 IST 字段指向这个 IST 索引。这也是 Linux 内核在启动早期就必做的一步。

---

## 参考资料

**Intel 手册（精确章节号）**：

- Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 3A
  - Section 3.4.2 — Segment Descriptors（GDT entry 结构和 Access Byte 字段定义）
  - Section 3.5.2 — System Descriptor Types（TSS 64-bit descriptor 格式）
  - Section 6.10 — Interrupt Descriptor Table (IDT)（IDT entry 结构和 Gate 类型定义）
  - Section 6.12.1 — Exception Classification（Trap / Fault / Abort 分类）
  - Section 6.14 — Control Transfers（IDT 向量号到 handler 的跳转机制）
  - Table 3-2 — System-Segment and Gate-Descriptor Types（TSS Available / Busy 的 type 值）
  - Table 6-1 — Protected-Mode Exceptions and Interrupts（所有异常向量号、类型和 error code 信息）

**AMD 手册**：

- AMD64 Architecture Programmer's Manual, Volume 2: System Programming
  - Section 4.6 — Descriptor Tables (GDTR, IDTR, LDTR, TR)
  - Section 4.7 — Interrupt and Exception Handling
  - Section 8.8 — Task State Segment (TSS)

**OSDev Wiki**：

- [GDT Tutorial](https://wiki.osdev.org/GDT_Tutorial) — GDT entry 结构详解和加载流程
- [IDT](https://wiki.osdev.org/IDT) — IDT entry 格式和 64-bit 模式下的变化
- [Interrupt Descriptor Table](https://wiki.osdev.org/Interrupt_Descriptor_Table) — Gate 类型（Interrupt/Trap/Task）的区别
- [ISR](https://wiki.osdev.org/ISR) — ISR stub 的编写方法和栈帧布局
- [Exceptions](https://wiki.osdev.org/Exceptions) — 所有 CPU 异常的向量号、error code 和触发条件汇总
- [TSS](https://wiki.osdev.org/Task_State_Segment) — TSS 结构体定义和 IST 机制
- [Interrupts](https://wiki.osdev.org/Interrupts) — x86 中断机制的总览

**其他参考资源**：

- OSDev Wiki 的 [Interrupt Frame](https://wiki.osdev.org/Interrupt_Frame) 页面，详细描述了 x86_64 下异常触发后的栈帧布局
- Philipp Oppermann 的 "Writing an OS in Rust" 系列博客中关于 [Double Fault](https://os.phil-opp.com/double-fault-exceptions/) 和 [IDT](https://os.phil-opp.com/cpu-exceptions/) 的章节，对 IST 和 Double Fault 处理有很好的图解
- Linux 内核源码 `arch/x86/kernel/idt.c`（较新版本用 `DEFINE_IDTENTRY` 宏声明式注册）和 `arch/x86/include/asm/desc_defs.h`（GDT/IDT entry 结构体定义）
