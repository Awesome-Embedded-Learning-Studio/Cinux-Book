# 007 Mini Kernel 中断系统 - 通读版

## 本章概览

这一章我们给 mini kernel 装上了完整的异常处理链路：GDT 提供段寄存器配置，IDT 提供中断向量表，ISR stub 用汇编接住异常并保存寄存器，最后 C 处理函数把完整的 CPU 状态打印到串口。从整个 OS 的启动链条来看，这一步处于"内存管理之后、磁盘驱动之前"的位置——有了异常处理，后续开发中任何内存错误都能被捕获而不是直接 triple fault 重启。

关键设计决策有三个：GDT 只设三项（null/code64/data64）足够满足 long mode 内核的需求；IDT 只配两个向量（#BP 和 #PF），因为当前阶段只需要断点调试和缺页检测；ISR stub 用宏生成而不是手写每个向量，方便将来扩展到 256 个。

和 xv6 对比的话，xv6 在进入内核的第一时间就配置了完整的 GDT 和 IDT，包含 TSS 和所有 256 个中断向量。我们这里采取了更渐进的方式——先跑通最小集合，等后续 milestone 需要硬件中断和用户态切换的时候再逐步填充。这种"最小可用子集"的策略在 OS 教学项目中很常见，好处是每一步的复杂度可控，坏处是中途可能需要回头改之前的结构。

---

## 架构图

```
异常触发流程（以 int $3 为例）：

mini_kernel_main()
       │
       │ asm volatile("int $3")
       ▼
   ┌─ CPU ─────────────────────────────────────┐
   │ 1. 查 IDT[3] → selector=0x08, offset=isr_bp_stub │
   │ 2. 查 GDT[1] → code64 segment 验证通过    │
   │ 3. 压栈: SS, RSP, RFLAGS, CS, RIP         │
   │ 4. 跳转到 isr_bp_stub                      │
   └────────────────────────────────────────────┘
       │
       ▼
   ┌─ isr_bp_stub (interrupts.S) ──────────────┐
   │ pushq $0          (伪错误码，保持栈对齐)    │
   │ pushq %rax ~ %r15 (保存 15 个通用寄存器)    │
   │ movq %rsp, %rdi   (InterruptFrame* → 参数) │
   │ call handle_bp    (跳转 C 处理函数)         │
   │ popq %r15 ~ %rax  (恢复寄存器)              │
   │ addq $8, %rsp     (弹出伪错误码)            │
   │ iretq             (返回被中断代码)           │
   └────────────────────────────────────────────┘
       │
       ▼
   ┌─ handle_bp (exception_handlers.cpp) ───────┐
   │ dump_interrupt_frame() → 串口打印寄存器     │
   │ kprintf 断点地址和提示信息                   │
   │ return → 回到 isr_bp_stub                    │
   └────────────────────────────────────────────┘
       │
       ▼
   继续执行 mini_kernel_main 后续代码

组件依赖关系：
   gdt_init() ──→ idt_init() ──→ 中断可用
       │              │
       │              └── IDT 条目引用 GDT 的 selector
       └── 必须先完成，IDT 依赖 GDT
```

---

## 关键代码精讲

### GDT：三项表，简洁但不可或缺

我们先看 GDT 的头文件，结构体定义本身并不复杂，但有几个常量值得注意。`SEGMENT_CODE64 = 1 * 8` 和 `SEGMENT_DATA64 = 2 * 8`，这个乘以 8 不是随便来的——段选择子的格式是 `[Index:TI:RPL]`，其中 Index 占 bit 15-3（共 13 位），TI 占 bit 2，RPL 占 bit 1-0。因为 TI=0（GDT）和 RPL=0（ring 0），所以选择子的值就等于索引乘以 8。这也是为什么 IDT 条目里的 selector 字段直接填 0x08 就能指向 GDT 的第二项（code64）。

```cpp
// gdt.hpp
struct GdtEntry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;
    uint8_t  flags_limit_high;
    uint8_t  base_high;
} __attribute__((packed));
```

`GdtEntry` 是 8 字节的 packed 结构，对应 x86 的 64 位段描述符。在 long mode 下，base 和 limit 字段被硬件忽略（除了 GS/FS 的 base 通过 MSR 设置），但 access 和 flags 字段仍然有效，CPU 会检查它们来决定段的类型和权限。

接下来看 `gdt_init()` 的实现，核心逻辑集中在三个 `make_gdt_entry` 调用上。第一项 null descriptor 全零，这是 x86 架构的硬性要求——CPU 不允许使用索引 0 对应的段，null descriptor 就是为了占这个坑。第二项 code64 的 access 是 `0x9A`，拆开来看：Present=1 表示段在内存中，DPL=00 表示 ring 0 内核态，S=1 表示这是一个代码/数据段（不是系统段），Type 位的 1010 表示可执行的、可读的代码段。flags 是 `0x0A`，即 G=1（4KB 粒度）和 L=1（64-bit long mode）——这个 L 位是整个 GDT 中最关键的一个 bit，它告诉 CPU 这是一个 64 位代码段。

```cpp
// gdt.cpp - 核心配置
s_gdt[0] = make_gdt_entry(0, 0, 0, 0);           // null
s_gdt[1] = make_gdt_entry(0, 0xFFFFF, 0x9A, 0x0A); // code64: L=1
s_gdt[2] = make_gdt_entry(0, 0xFFFFF, 0x92, 0x0C); // data64: D/B=1
```

data segment 的 access 是 `0x92`，和 code 的区别在于 Type 位：0x92 的 bit 3（Executable）是 0，表示这是一个数据段而非代码段。flags 是 `0x0C`，即 G=1 和 D/B=1，注意在 long mode 下 data segment 的 L 位被硬件忽略，D/B 位也基本被忽略，所以填 0x0C 或 0x04 都能工作，我们选择 0x0C 是因为这是大多数内核项目的惯例。

加载 GDT 的汇编部分用了一个 far return 的技巧来刷新 CS 寄存器。单纯执行 `lgdt` 只修改了 GDTR，但 CPU 内部的 CS 缓存不会自动更新，所以需要一次"远跳转"来强制 CPU 重新加载 CS。这里选择 `lretq` 而不是 `ljmp`，是因为在 higher-half kernel 里 `ljmp` 需要一个绝对地址，而 `lretq` 可以用栈上的相对地址，对位置无关代码更友好。DS/ES/FS/GS/SS 则不需要这种技巧，直接 `mov` 赋值就行。

### IDT：256 槽的空房子，只住了两个住户

IDT 的结构体比 GDT 大一些——每个条目 16 字节而不是 8 字节，因为需要存放 64 位的处理程序地址。地址被拆成了三段（offset_low 16 位 + offset_mid 16 位 + offset_high 32 位），这是 x86_64 的 IDT 格式决定的，没法用一条指令设置完整地址，必须手动拆分。

```cpp
// idt.hpp
struct IdtEntry {
    uint16_t offset_low;    // [0:15]
    uint16_t selector;      // CS 选择子
    uint8_t  ist;           // IST 偏移
    uint8_t  type_attr;     // P | DPL | 0 | Gate Type
    uint16_t offset_mid;    // [16:31]
    uint32_t offset_high;   // [32:63]
    uint32_t reserved;
} __attribute__((packed));
```

`InterruptFrame` 是整个中断处理链路中最关键的数据结构。前 15 个字段（r15 到 rax）由 ISR stub 手动保存，error_code 也是 stub 处理的（有硬件错误码的就保留 CPU 压入的，没有的就压一个 0），最后 5 个字段（rip 到 ss）是 CPU 自动压入的。这个布局必须和 `interrupts.S` 中的 push 顺序严格对应——任何错位都会导致 C 处理函数读到错误的寄存器值。

`idt_init()` 的实现很直白：先清空 256 个条目，然后只配置 #BP 和 #PF 两个向量。#BP 用陷阱门（type_attr = 0x8F），这意味着进入处理程序时 IF 不被清除，允许嵌套中断；#PF 用中断门（0x8E），CPU 会自动关中断。两者都是 Present=1、DPL=0，只有 ring 0 能触发。

### ISR Stub：汇编层面的寄存器搬运工

`interrupts.S` 定义了两个宏——`ISR_NOERRCODE` 给没有硬件错误码的异常（如 #BP），`ISR_ERRCODE` 给有硬件错误码的异常（如 #PF）。两者的区别很小：NOERRCODE 版本在保存寄存器之前先 `pushq $0` 填一个伪错误码，ERRCODE 版本跳过这步因为 CPU 已经帮你压了。

寄存器的保存顺序需要特别注意。我们按 rax → rbx → rcx → ... → r15 的顺序 push，但 `InterruptFrame` 结构体的字段声明是 r15 → ... → rax。这看起来矛盾，其实是对的——栈是从高地址往低地址增长的，第一个 push 的值在高地址（栈底），最后一个 push 的值在低地址（栈顶，即 RSP 指向的位置）。而结构体的第一个字段在最低地址。所以最后 push 的 r15 对应结构体的第一个字段 r15，第一个 push 的 rax 对应结构体中间的 rax 字段。

调用 C 函数之前的 `movq %rsp, %rdi` 把当前栈指针传给 C 函数作为第一个参数（System V ABI 规定第一个参数用 RDI）。此时 %rsp 指向的就是 InterruptFrame 结构体的起始位置，C 代码可以用 `frame->r15` 这样的语法直接访问每个保存的寄存器。

恢复过程是保存的镜像：先 pop r15 → ... → pop rax，然后 `addq $8, %rsp` 跳过错误码（不管它是 CPU 压的还是 stub 填的伪值），最后 `iretq` 从栈上弹出 CPU 压入的五个值（RIP/CS/RFLAGS/RSP/SS），恢复到被中断的代码继续执行。

### 异常处理函数：串口上的"遗言"

`handle_bp` 的实现很简洁：调用 `dump_interrupt_frame` 打印完整的寄存器快照，然后输出一条提示说这是软件断点可以安全继续。因为 #BP 是陷阱类型（trap），CPU 压入的 RIP 指向 `int $3` 的下一条指令，所以 `iretq` 返回后执行不会重复触发。

`handle_pf` 多了一步：读取 CR2 寄存器获取导致缺页的线性地址。CR2 是 x86 为页错误专门保留的寄存器——CPU 在触发 #PF 之前会自动把出问题的地址写入 CR2，这比从 RIP 猜测出错位置要准确得多。错误码的解析也很有信息量：bit 0 区分"页不存在"和"权限冲突"，bit 1 区分读还是写，bit 2 区分内核态还是用户态。当前我们的处理策略是打印完就继续——虽然这不能"修复"缺页，但至少不会让内核静默死掉。

---

## 设计决策深度分析

### 决策一：ISR Stub 用宏生成，而不是为每个向量手写

**问题**：x86_64 有 256 个中断向量，将来需要为大部分向量提供 ISR stub。如果手写每个 stub，代码量会爆炸且难以维护。

**本项目的做法**：定义 `ISR_NOERRCODE` 和 `ISR_ERRCODE` 两个宏，需要时一行代码实例化一个 stub。宏内部处理寄存器保存/恢复、错误码对齐、C 函数调用等通用逻辑。

**备选方案**：用一个统一的 stub 入口，通过 CPU 自动压入的向量号来分发。Linux 就是这么做的——所有异常入口跳到同一段汇编代码，从栈上读取向量号然后查表调用对应的处理函数。

**为什么不选备选方案**：统一入口的方案更紧凑，但调试更困难——GDB 断点只能打在公共入口，不能直接定位到具体向量。对于教学项目来说，每个向量有独立符号（如 `isr_bp_stub`）更有利于理解。将来如果需要扩展到更多向量，可以在保留当前宏的基础上，增加一个批量生成的辅助宏。

**如果要扩展/改进**：在大内核（milestone 010+）中，我们会实现完整的 256 向量 IDT，届时可以写一个循环式的宏来批量生成所有 stub，类似于 Linux 的 `__ENTRY` 宏。

### 决策二：#BP 用陷阱门，#PF 用中断门

**问题**：中断门和陷阱门的唯一区别是进入处理程序时是否清除 IF（中断允许标志），选择哪种门影响嵌套中断行为。

**本项目的做法**：#BP 用陷阱门（不清除 IF），#PF 用中断门（清除 IF）。

**备选方案**：全部用中断门或全部用陷阱门。

**为什么不选备选方案**：#BP 是调试用的断点异常，处理过程中如果禁止其他中断，可能会错过时序敏感的事件（比如时钟中断），所以用陷阱门更合理。#PF 涉及页表操作，这是一个需要原子性的过程——如果处理到一半被另一个中断打断，可能导致页表状态不一致，所以用中断门保护。Intel 手册也是这么建议的。

**如果要扩展/改进**：当加入外部硬件中断（IRQ0-15）后，所有硬件中断都应该用中断门，因为中断处理过程中需要先发 EOI 再开中断，不能被嵌套打断。

### 决策三：异常处理只打印不修复

**问题**：#PF 等异常在实际 OS 中需要修复（如按需分配缺页），当前阶段是否应该实现修复逻辑。

**本项目的做法**：只打印信息然后继续执行。对于 #BP 这没问题（陷阱类型，RIP 已指向下一条指令），但对于 #PF 这意味着如果代码真的访问了无效地址，会反复触发 #PF 形成死循环。

**备选方案**：在 #PF handler 中检查 CR2，如果是合理的地址就分配页表并映射。

**为什么不选备选方案**：当前 mini kernel 还没有虚拟内存管理器（VMM），PMM 只管理物理页，没有页表映射的能力。在 milestone 016 实现 VMM 之后，#PF handler 才有修复缺页的基础设施。现在强行实现只会引入没有意义的 stub 代码。

**如果要扩展/改进**：在 milestone 016（VMM）中，#PF handler 会变成按需分配的核心——检查 CR2 对应的虚拟地址是否在合法范围内，如果是就调 VMM::map 分配新页。

---

## 常见变体与扩展方向

**⭐ 为所有 x86 异常向量配置 IDT 条目**：除了 #BP 和 #PF，还有 #DE（除零）、#GP（一般保护错误）、#DF（双重错误）等常用向量。每个向量配一个 ISR stub 和对应的处理函数，可以在开发过程中捕获更多类型的错误。难度不高，主要是体力活。

**⭐⭐ 实现 TSS（Task State Segment）和 IST**：TSS 在 x86_64 下的主要用途是提供特权级切换时的内核栈指针（RSP0），以及 IST（Interrupt Stack Table）提供的独立中断栈。加入 TSS 后，#DF 可以使用 IST1 指向的独立栈，避免在内核栈已经损坏的情况下处理双重错误。这需要扩展 GDT 到 5 项以上（加上 TSS 描述符占两个 slot）。

**⭐⭐ 加入外部硬件中断（PIC/IRQ）**：配置 8259A PIC，把 IRQ0-15 映射到 IDT 向量 32-47，实现时钟中断和键盘中断。这是 milestone 011 的内容，需要在 ISR stub 宏基础上增加 16 个硬件中断的 stub，以及 PIC 的初始化和 EOI 发送逻辑。

**⭐⭐⭐ 实现 APIC 替代 8259A**：现代 x86 系统使用 Local APIC 和 I/O APIC 代替传统 8259A PIC。APIC 支持更多中断向量（256 个全部可用）、多处理器中断路由、优先级管理等高级特性。这个扩展需要读写 MSR 和 MMIO 寄存器，难度较高但非常贴近实际内核开发。

---

## 参考资料

- **Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 3A**
  - Section 3.4.4: Segment Descriptors（GDT 描述符格式）
  - Section 3.5.2: Segment Selection（段选择子格式）
  - Section 6.10: Interrupt Descriptor Table（IDT 格式）
  - Section 6.12: Exception and Interrupt Handling（异常处理流程）
  - Section 6.13: Error Codes（错误码格式，特别是 #PF 错误码）

- **AMD64 Architecture Programmer's Manual, Volume 2: System Programming**
  - Section 4.7: Long-Mode Segment Descriptors
  - Section 8.2: IDT and Gate Descriptors in Long Mode

- **OSDev Wiki**
  - [GDT Tutorial](https://wiki.osdev.org/GDT_Tutorial)
  - [IDT](https://wiki.osdev.org/IDT)
  - [Interrupts](https://wiki.osdev.org/Interrupts)
  - [Exceptions](https://wiki.osdev.org/Exceptions)
  - [ISR](https://wiki.osdev.org/Interrupt_Service_Routines)

- **其他参考**
  - xv6-public 源码：`trap.c` / `trapasm.S` / `mmu.h`（MIT 经典教学内核的中断实现）
  - Linux 内核 `arch/x86/entry/entry_64.S`（生产级 x86_64 中断入口）
