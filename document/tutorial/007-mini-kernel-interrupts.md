# 从零写个中断系统：让内核不再"一句话不说就死"

> 标签：x86-64, 中断, GDT, IDT, ISR, 裸机开发, C++, AT&T 汇编, QEMU, OS 开发

## 前言

说实话，搞 OS 开发最让人崩溃的事情不是"代码写不出来"，而是"写出来之后一跑就黑屏，连个遗言都留不下"。在之前几个 milestone 里，我们的小内核已经能启动、能打印信息、能管理物理内存了，但如果 CPU 遇到任何异常——比如除零、访问无效地址、执行非法指令——它会直接 triple fault 然后 QEMU 重启，你什么信息都看不到。

这就像调试一个连 print 都没有的程序：你只知道"它挂了"，但完全不知道"为什么挂了"、"挂在哪里"。这种体验真的让人血压拉满，尤其是在后续开发大内核的时候，内存错误会越来越频繁，没有异常处理就等于闭着眼睛写代码。

所以这一章我们要做的事情非常明确：给 mini kernel 装上 GDT（全局描述符表）和 IDT（中断描述符表），让 CPU 遇到异常的时候能跳到我们的处理函数，把完整的寄存器状态打印到串口，然后继续执行而不是直接死掉。完成之后的效果是这样的——我们在 `mini_kernel_main` 里故意触发一个断点异常 `int $3`，串口会输出完整的寄存器 dump，然后内核继续运行，不死机，不重启。

## 环境说明

老规矩，先交代一下实验环境：x86_64 平台，工具链是 GNU AS（AT&T 语法）+ GCC/G++ + CMake，跑在 QEMU 里。内核是 freestanding 的 C++23，没有标准库、没有异常、没有 RTTI。当前我们处于 mini kernel 阶段，内核被 Bootloader 加载到 0x20000（物理地址），运行在 higher-half 虚拟地址空间（0xFFFFFFFF80000000 起始）。

一个很关键的约束是 `-mno-red-zone`——这个编译选项告诉我们不能使用 x86_64 的 red zone（栈顶以下 128 字节的"安全区域"），因为中断可能在任何时刻发生，如果用了 red zone，中断处理程序的栈操作会覆盖掉被中断函数的局部变量。在内核开发中这个选项是必须的。

## 第一步——搞清楚 CPU 遇到异常时会做什么

在写代码之前，我们先搞明白 x86_64 的异常处理机制。这不是我照本宣科念手册，而是我们在实际开发中必须理解的底层行为——不理解这些的话，后面写 ISR stub 的时候会觉得每个 push 都是玄学。

当 CPU 检测到一个异常（比如执行了 `int $3` 指令），它会做以下事情：

首先，CPU 会根据异常的向量号（#BP 是 3，#PF 是 14）去查 IDT——Interrupt Descriptor Table，一个最多 256 项的表，每一项告诉 CPU"遇到这个向量号应该跳到哪里处理"。IDT 条目里有一个 selector 字段，指向 GDT 中的代码段描述符，CPU 用这个 selector 来验证"跳转目标是不是合法的代码段"。

验证通过后，CPU 会把当前的 SS、RSP、RFLAGS、CS、RIP 依次压入栈中。注意这个顺序是固定的，从高地址到低地址：SS 在最上面（高地址），RIP 在最下面（低地址）。如果这个异常会产生错误码（比如 #PF），CPU 还会额外压入一个错误码，放在 RIP 上面。

然后 CPU 从 IDT 条目中取出处理程序的地址，跳过去执行。处理程序结束后执行 `iretq` 指令，CPU 从栈上弹出 RIP、CS、RFLAGS、RSP、SS，恢复到被中断的代码。

```
异常触发时 CPU 的压栈操作（从高地址到低地址）：

高地址 ──┐
         │  SS          ← CPU 压入
         │  RSP         ← CPU 压入
         │  RFLAGS      ← CPU 压入
         │  CS          ← CPU 压入
         │  RIP         ← CPU 压入
         │  Error Code  ← CPU 压入（仅部分异常）
         ├─── ISR stub 保存 ───
         │  RAX ~ R15   ← 我们的代码压入
低地址 ──┘  ← RSP 指向这里
```

你会发现 CPU 只保存了 5 个寄存器（加上可选的错误码），其余 16 个通用寄存器的值全丢了。所以我们的 ISR stub 必须在调用 C 处理函数之前，把这 16 个寄存器全部保存到栈上，这样 C 代码才能读到异常发生时的完整 CPU 状态。

## 第二步——搭建 GDT：段寄存器的配置表

GDT 这个东西，说实话在现代 x86_64 上存在感已经很弱了。在 16 位和 32 位时代，分段机制是内存管理的核心——每个段有独立的基地址和限长，程序员需要手动管理段寄存器。但到了 64 位 long mode，硬件基本忽略段的 base 和 limit（除了 GS/FS），内存保护完全由分页机制负责。

那为什么我们还要设置 GDT？原因有两个。第一，CS/DS/SS 这些段寄存器仍然必须指向一个有效的 GDT 条目，CPU 每次访问内存都会用段选择子去查 GDT，查不到或者查到无效的描述符就触发 General Protection Fault。第二，我们的 IDT 条目里有一个"代码段选择子"字段，中断触发时 CPU 用这个选择子来加载 CS——如果 GDT 没配好，中断一触发就 triple fault。

我们的 GDT 只需要三项就够了：第一项是 null descriptor（全零，x86 架构硬性要求索引 0 不可用），第二项是 64-bit code segment，第三项是 64-bit data segment。code segment 的 access byte 是 `0x9A`，展开成二进制是 `10011010`——Present=1（段存在）、DPL=00（ring 0 内核态）、S=1（代码/数据段）、Type=1010（可执行可读的代码段）。flags 是 `0x0A`，关键是 L 位（Long mode）必须为 1，这告诉 CPU 这是一个 64 位代码段。

data segment 的 access 是 `0x92`——和 code 的区别只有 Type 字段：`0010` 表示可读写的数据段。flags 是 `0x0C`，在 long mode 下 data segment 的 L 位和 D/B 位都被硬件忽略，填什么都能工作，`0x0C` 是大多数内核项目的惯例。

加载 GDT 有个小技巧值得一提。`lgdt` 指令只修改 GDTR 寄存器，但 CPU 内部的 CS 缓存不会自动更新。我们需要一次"远跳转"来强制 CPU 重新读取 CS 的描述符。这里用 `lretq`（far return）来实现：先把新的 CS 选择子和返回地址压栈，然后执行 `lretq`，CPU 就从栈上弹出新的 CS 和 RIP，完成段寄存器刷新。DS/ES/FS/GS/SS 不需要这种花活，直接 `mov` 赋值就行。

## 第三步——搭建 IDT：两个向量的电话簿

IDT 是一个最多 256 项的表，每项 16 字节，包含中断处理程序的地址、代码段选择子、IST 偏移和类型属性。我们当前只配两个向量：#BP（向量 3，断点异常）和 #PF（向量 14，页错误）。

#BP 用的是陷阱门（type_attr = 0x8F），#PF 用的是中断门（0x8E）。两者的区别只有一个：CPU 跳转到中断门处理程序时会自动清除 RFLAGS.IF（关中断），跳转到陷阱门时不会。#BP 是调试用的断点，处理过程中如果禁止中断会错过时钟中断等时序敏感事件，所以用陷阱门。#PF 涉及页表操作需要原子性，用中断门防止嵌套中断导致页表状态不一致。

IDT 条目的 16 字节布局中，处理程序的 64 位地址被拆成三段存放（低 16 位 + 中 16 位 + 高 32 位），这是 x86_64 的硬件格式决定的。我们的 `set_idt_entry` 函数做的就是把这个地址拆开分别填入对应字段，再把 selector 设为 `SEGMENT_CODE64`（0x08），type_attr 设为对应的门类型。清空 IDT 时我们把 256 个条目全部置零——Present=0 的条目表示"未使用"，CPU 触发未配置的向量时会触发 General Protection Fault。

## 第四步——ISR Stub：汇编里的寄存器搬运工

ISR stub 是整个中断链路中汇编密度最高的部分，也是最容易出错的地方。我们定义了两个宏——`ISR_NOERRCODE` 处理不产生硬件错误码的异常（如 #BP），`ISR_ERRCODE` 处理有硬件错误码的异常（如 #PF）。

两者的核心区别只有一个：NOERRCODE 版本在保存寄存器之前先 `pushq $0` 填一个伪错误码。这看似多余，但对 C 处理函数至关重要——`InterruptFrame` 结构体有一个 `error_code` 字段，如果没有伪错误码，这个字段的位置会被 CPU 压入的 RIP 占据，整个结构体全部错位，C 代码读到的所有寄存器值都是错的。这个坑真的很隐蔽，很多新手第一次写 ISR 的时候都在这里栽过——异常处理函数打印出来的寄存器值完全不合理，但编译又没错，调试半天才发现是栈帧错位了。

寄存器的 push 顺序也需要和 `InterruptFrame` 结构体的字段声明严格对应。结构体从上到下声明 r15、r14、...、rax，但 push 是先 rax、再 rbx、最后 r15。这是因为栈从高地址往低地址增长——先 push 的在高地址（栈底），后 push 的在低地址（栈顶，即 RSP 指向的位置），而结构体的第一个字段在最低地址。所以最后 push 的 r15 刚好对应结构体的第一个字段。

保存完寄存器后，`movq %rsp, %rdi` 把栈指针传给 C 函数作为第一个参数。此时 %rsp 指向的就是 InterruptFrame 的起始地址，C 代码可以用 `frame->rip` 这样的语法直接读到异常发生时的指令地址。处理完成后，逆序 pop 恢复所有寄存器，`addq $8, %rsp` 跳过错误码，最后 `iretq` 返回被中断的代码。

## 第五步——C 处理函数和 int $3 点火测试

C 处理函数就比较轻松了。`handle_bp` 调用一个辅助函数 `dump_interrupt_frame` 打印完整寄存器快照，然后输出提示信息。`handle_pf` 多了一步——读取 CR2 寄存器获取导致缺页的线性地址，再解析错误码的各个 bit 位（页不存在 vs 权限冲突、读 vs 写、内核 vs 用户）。这些信息在调试缺页问题的时候非常关键。

两个函数都是 `extern "C"` 声明的——因为 `interrupts.S` 里的 `call handle_bp` 使用 C 链接约定，如果用 C++ 的 name mangling 的话链接器找不到符号。这是内核开发中混合 C/C++/汇编时的标准做法。

最后在 `mini_kernel_main` 里，我们把整个链路串起来：

```
gdt_init() → idt_init() → pmm::init() → asm volatile("int $3") → 继续执行
```

初始化顺序不能错：GDT 必须在 IDT 之前，因为 IDT 条目引用了 GDT 中的代码段选择子。然后我们用 `int $3` 触发断点异常——如果一切配置正确，串口会打印完整的寄存器 dump，然后内核继续执行后续代码。

## 上板验证

构建运行后，串口输出的关键部分长这样：

```
[INIT] Setting up GDT...
[INIT] GDT loaded successfully.
[INIT] Setting up IDT...
[INIT] IDT loaded successfully.

[TEST] Triggering breakpoint exception (int $3)...

==== EXCEPTION: #BP (vector 3) ====
  RIP   = 0xffffffff80020224   CS  = 0x0008
  RFLAGS= 0x0000000000000046
  RSP   = 0xffffffff80025100   SS  = 0x0010
  RAX=0x0000000000000000  RBX=0x0000000000004118
  ...（其余寄存器）
  ERROR CODE = 0x0000000000000000
========================================
[EXCEPTION] Breakpoint triggered at RIP=0xffffffff80020224
[EXCEPTION] This is a software breakpoint, continuing...
[TEST] Breakpoint test passed! Execution continued after #BP.
```

CS = 0x0008 对应 GDT 索引 1（0x08 / 8 = 1），就是我们设的 code64 segment。SS = 0x0010 对应索引 2，是 data64 segment。RIP 指向 `int $3` 下一条指令的地址——因为 #BP 是陷阱类型，CPU 压入的 RIP 已经指向下一条指令了。看到 `[TEST] Breakpoint test passed!` 就说明整个链路通畅：GDT → IDT → ISR stub → C handler → iretq → 继续执行。

Host 端单元测试也全部通过（40 个用例覆盖 GDT/IDT 描述符编码、结构体布局、段选择子常量），QEMU 内核端测试 6 passed / 0 failed。

## 踩坑总结

写这个 milestone 的过程中踩了几个坑，记一下免得以后再踩：

**栈帧错位**。如果 #BP 的 ISR stub 忘了压伪错误码，`InterruptFrame` 的 error_code 字段会读到 CPU 压入的 RIP，然后 rip 字段读到 CS，整个 dump 全是错的。debug 方法是对照汇编 push 顺序和 C 结构体字段顺序，一个一个数偏移量。

**GDT 必须在 IDT 之前初始化**。如果反过来，`lidt` 能成功但中断触发时 CPU 拿 selector 去查 GDT 会查到 null descriptor（Present=0），触发 #GP，然后 #GP 也没配处理，就 triple fault 了。这个顺序在代码里看起来很显然，但在重构的时候不小心调换顺序就会中招。

**higher-half 地址**。GDT 和 IDT 的 `GdtPointer.base` / `IdtPointer.base` 需要填虚拟地址（因为内核运行在 higher-half），不是物理地址。如果填了物理地址，CPU 尝试在物理地址空间读取描述符表，在 higher-half 映射下这个物理地址可能不可访问，直接 triple fault。

## 到这里就大功告成了

现在我们的 mini kernel 有了一个可用的异常处理基础设施。触发异常不再是无声的死亡——它会告诉你 RIP 在哪里、RSP 是多少、各个寄存器的值是什么。这在后续开发中的价值会越来越大：每当我们写了一段新的代码导致内核崩溃，串口上的寄存器 dump 就是排查问题的第一手线索。

下一章我们会实现 ATA PIO 磁盘驱动和 ELF 加载器，让 mini kernel 能从磁盘加载大内核并跳转执行。到时候 IDT 中的 #PF handler 就能真正发挥作用了——大内核加载过程中的内存访问错误会被立即捕获并报告，而不是静默地 triple fault。
