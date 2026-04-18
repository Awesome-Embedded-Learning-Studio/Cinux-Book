# 007 Mini Kernel 中断系统 - 让内核学会"接电话"

## 章节导语

上一章我们搞定了物理内存管理器（PMM），内核终于能分配和释放内存了。但说实话，目前这个内核还是一个"聋子"——CPU 遇到异常（比如除零、缺页、断点）的时候，只会茫然地 triple fault 然后重启，连个遗言都留不下。这在开发阶段简直是灾难：你写了半天的代码，跑起来直接黑屏，连哪里出的问题都不知道。

所以这一章我们要给内核装上"耳朵和嘴巴"——全局描述符表（GDT）和中断描述符表（IDT）。GDT 告诉 CPU 段寄存器该怎么设置，IDT 告诉 CPU 遇到异常该找谁处理。完成本章后，我们会在 mini_kernel_main 里用一条 `int $3` 指令故意触发断点异常，然后在串口看到完整的寄存器 dump——触发异常不死机，能看到错误信息，这就是本章的目标。

本章的前置知识是上一章（006_mini_kernel_pmm）的 PMM 初始化流程，以及对 x86 段寄存器和栈操作的基本了解。

---

## 概念精讲

### 为什么需要 GDT？上一章不是已经在 protected mode/long mode 下了吗？

这是个好问题。Bootloader 确实在进入 long mode 的时候设置过一次 GDT，但那是 Bootloader 自己的 GDT，放在低地址区域。现在内核已经接管了控制权，我们需要在自己的地址空间里重新建立一套 GDT，确保段寄存器指向正确的描述符。

你可以把 GDT 理解为一张"段寄存器配置表"。在 x86_64 的 long mode 下，分段机制已经被大幅弱化——基地址和限长基本被硬件忽略了，但 CS/DS/SS 这些段寄存器仍然需要指向有效的 GDT 条目，CPU 才能正常工作。尤其是中断处理（IDT 里的每个条目都有一个"代码段选择子"字段，指向 GDT 中的 code segment），如果 GDT 没配好，中断一触发就 triple fault。

```
CPU 触发异常时的查找链路：
CPU 异常 → 查 IDT[向量号] → 得到 selector + handler 地址
                                   ↓
                            查 GDT[selector] → 得到 code segment 属性
                                   ↓
                            跳转到 handler 执行
```

### IDT 是什么？Interrupt Gate 和 Trap Gate 有什么区别？

IDT（Interrupt Descriptor Table）是 x86 架构的"中断电话簿"。CPU 遇到异常或收到外部中断时，会拿着一个向量号（0-255）去 IDT 里查对应的处理程序地址，然后跳过去执行。

对我们这个 milestone 来说，只需要配置两个向量就够了：
- **向量 3（#BP）**：断点异常，`int $3` 指令触发
- **向量 14（#PF）**：页错误异常，访问无效内存地址时触发

IDT 里有两种门（Gate）：中断门（Interrupt Gate，类型 0xE）和陷阱门（Trap Gate，类型 0xF）。它们的区别只有一个：跳转到中断门处理程序时，CPU 会自动清除 RFLAGS.IF 标志（关中断），而陷阱门不会。对于 #BP 这种调试用的异常，我们用陷阱门，这样断点处理期间仍然能响应其他中断；对于 #PF，我们用中断门，因为页错误的处理过程不应该被其他中断打断。

### 中断栈帧（Interrupt Frame）是怎么回事？

当 CPU 响应异常时，它会自动把当前的 SS、RSP、RFLAGS、CS、RIP 压入栈中（注意顺序是从高地址到低地址）。对于某些异常（比如 #PF），CPU 还会额外压入一个错误码。然后 CPU 跳转到 IDT 中指定的处理程序地址。

我们的 ISR stub（汇编写的）在 CPU 压栈的基础上，再把所有通用寄存器保存到栈上，加上一个统一的错误码字段（没有硬件错误码的异常就填 0），这样就形成了一个完整的 `InterruptFrame` 结构体。C 处理函数收到这个结构体的指针，就能读取异常发生时的完整 CPU 状态。

```
中断发生时的栈布局（从高地址到低地址）：
┌──────────────────┐  ← 高地址
│  SS              │  CPU 自动压入
│  RSP             │
│  RFLAGS          │
│  CS              │
│  RIP             │
│  Error Code (可选)│  CPU 压入（#PF 有，#BP 没有）
├──────────────────┤  ← ISR stub 保存
│  RAX             │
│  RBX             │
│  ...             │
│  R15             │
│  Dummy Error Code│  #BP 才有（stub 填 0）
└──────────────────┘  ← RSP 指向这里
```

### AT&T 汇编语法速查

我们的 ISR stub 用 GNU AS（AT&T 语法）编写，几个关键区别需要记住：
- 操作数顺序是 `源, 目标`（和 Intel 语法相反）：`movq %rax, %rbx` 是把 RAX 的值放到 RBX
- 寄存器前缀 `%`，立即数前缀 `$`：`pushq $0` 是压入数字 0
- 内存寻址格式 `offset(base)`：`8(%rsp)` 是访问 RSP+8 处的内存

---

## 动手实现

### 第一步——搭建 GDT：内核的段寄存器配置表

**目标**：创建一个最简 GDT，包含 null/code64/data64 三项描述符，然后通过 `lgdt` 加载并刷新所有段寄存器。

**代码**（`kernel/mini/arch/x86_64/gdt.hpp`）：

```cpp
// GDT 常量
constexpr uint8_t GDT_ENTRIES = 3;
constexpr uint16_t SEGMENT_CODE64 = 1 * 8;  // 索引 1 × 8 + RPL=0
constexpr uint16_t SEGMENT_DATA64 = 2 * 8;  // 索引 2 × 8 + RPL=0

struct GdtEntry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;           // 访问权限字节
    uint8_t  flags_limit_high; // 高4位flags + 低4位limit高4位
    uint8_t  base_high;
} __attribute__((packed));

struct GdtPointer {
    uint16_t limit;  // GDT 字节大小 - 1
    uint64_t base;   // GDT 的线性地址
} __attribute__((packed));

void gdt_init();
```

**代码**（`kernel/mini/arch/x86_64/gdt.cpp`，关键部分）：

```cpp
void gdt_init() {
    // null descriptor：全零，CPU 规定第一项必须如此
    s_gdt[0] = make_gdt_entry(0, 0, 0, 0);

    // 64-bit code segment
    // Access = 0x9A: Present=1, DPL=00, S=1, Code=1, RW=1
    // Flags  = 0x0A: G=1, L=1 (long mode 标志位，这是关键)
    s_gdt[1] = make_gdt_entry(0, 0xFFFFF, 0x9A, 0x0A);

    // 64-bit data segment
    // Access = 0x92: Present=1, DPL=00, S=1, Data=0, RW=1
    // Flags  = 0x0C: G=1, D/B=1
    s_gdt[2] = make_gdt_entry(0, 0xFFFFF, 0x92, 0x0C);

    // 加载 GDTR 并刷新段寄存器
    s_gdt_pointer.limit = sizeof(s_gdt) - 1;
    s_gdt_pointer.base  = (uint64_t)&s_gdt;

    __asm__ volatile (
        "lgdt %[gdtr]\n\t"          // 加载 GDT
        "pushq %[cs]\n\t"           // far return 刷新 CS
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"                 // ← CS 被刷新为 SEGMENT_CODE64
        "1:\n\t"
        "movw %[ds], %%ax\n\t"      // 重载 DS/ES/FS/GS/SS
        "movw %%ax, %%ds\n\t"
        // ... 其余段寄存器
        : : [gdtr] "m" (s_gdt_pointer),
            [cs] "i" (SEGMENT_CODE64),
            [ds] "i" (SEGMENT_DATA64)
        : "rax", "memory"
    );
}
```

这里有几个值得说的点。首先是 Access Byte 的编码——`0x9A` 拆开来看是 `10011010b`，从高位到低位依次是 Present（1）、DPL（00）、Descriptor（1）、Executable（1）、Direction（0）、ReadWrite（1）、Accessed（0），这些位告诉 CPU 这是一个"存在于内存中的、ring 0 的、可执行的、可读的代码段"。然后是 Flags 的 L 位（Long mode）——这是 64-bit code segment 的标志，必须为 1，否则 CPU 不知道这是 64 位代码。data segment 则不需要 L 位，它用 D/B 位。

关于加载过程，`lgdt` 只是修改了 GDTR 寄存器，但 CPU 内部的 CS 缓存并不会因此更新，所以我们用了一个 `lretq`（far return）的技巧：先把新的 CS 选择子压栈，再把返回地址压栈，然后执行 `lretq`，CPU 就会从栈上弹出新的 CS 和 RIP，完成段寄存器的刷新。DS/ES/FS/GS/SS 则直接用 `mov` 赋值就行，不需要 far jump。

**验证**：构建运行后应该看到 `[INIT] GDT loaded successfully.`。

---

### 第二步——搭建 IDT：中断向量表

**目标**：创建一个 256 项的 IDT，配置 #BP(3) 和 #PF(14) 两个异常向量，通过 `lidt` 加载。

**代码**（`kernel/mini/arch/x86_64/idt.hpp`，关键结构）：

```cpp
struct IdtEntry {
    uint16_t offset_low;    // handler 地址 [0:15]
    uint16_t selector;      // 代码段选择子（CS）
    uint8_t  ist;           // IST 偏移（0 = 不用）
    uint8_t  type_attr;     // P | DPL | 0 | Gate Type
    uint16_t offset_mid;    // handler 地址 [16:31]
    uint32_t offset_high;   // handler 地址 [32:63]
    uint32_t reserved;
} __attribute__((packed));

struct InterruptFrame {
    uint64_t r15, r14, r13, r12;
    uint64_t r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rdx;
    uint64_t rcx, rbx, rax;
    uint64_t error_code;  // #BP: stub 填 0; #PF: CPU 自动填
    uint64_t rip, cs, rflags, rsp, ss;  // CPU 自动压入
};

void idt_init();
```

IDT 条目在 x86_64 下是 16 字节的，因为 handler 地址是 64 位的，被拆成了三段存放（offset_low / offset_mid / offset_high）。selector 字段指向 GDT 中的代码段——这里填的就是我们上一步设置的 `SEGMENT_CODE64`（0x08），这就是为什么 GDT 必须先于 IDT 初始化。

type_attr 字段的低 4 位是门类型：`0xE` 是中断门，`0xF` 是陷阱门。我们给 #BP 用陷阱门（0x8F），给 #PF 用中断门（0x8E），高 4 位中的 Present 位必须为 1，否则 CPU 会触发 General Protection Fault。

**代码**（`kernel/mini/arch/x86_64/idt.cpp`，关键配置）：

```cpp
void idt_init() {
    // 清空全部 256 项
    for (uint16_t i = 0; i < 256; i++)
        s_idt[i] = IdtEntry{};

    // #BP(3) - 陷阱门
    set_idt_entry(3, (void*)isr_bp_stub, SEGMENT_CODE64, 0x8F, 0);
    // #PF(14) - 中断门
    set_idt_entry(14, (void*)isr_pf_stub, SEGMENT_CODE64, 0x8E, 0);

    // 加载 IDTR
    s_idt_pointer.limit = sizeof(s_idt) - 1;
    s_idt_pointer.base  = (uint64_t)&s_idt;
    __asm__ volatile ("lidt %[idtr]" : : [idtr] "m" (s_idt_pointer) : "memory");
}
```

**验证**：应该看到 `[INIT] IDT loaded successfully.`。

---

### 第三步——ISR Stub：用汇编接住异常

**目标**：编写 ISR stub，保存所有通用寄存器，构造 InterruptFrame，调用 C 处理函数，然后恢复并返回。

**代码**（`kernel/mini/arch/x86_64/interrupts.S`，关键逻辑）：

这里有两个宏——`ISR_NOERRCODE` 给 #BP 这种没有硬件错误码的异常用，`ISR_ERRCODE` 给 #PF 这种 CPU 自动压入错误码的异常用。

```asm
# 无错误码的异常（#BP）
.macro ISR_NOERRCODE name vector handler
\name:
    pushq $0              /* 压入伪错误码 0，保持栈帧统一 */
    pushq %rax            /* 保存所有 16 个通用寄存器 */
    pushq %rbx
    /* ... r15 ... */
    movq %rsp, %rdi       /* InterruptFrame* 作为第一个参数 */
    call \handler         /* 调用 C 处理函数 */
    popq %r15             /* 恢复寄存器（反向弹出） */
    /* ... rax ... */
    addq $8, %rsp         /* 弹出伪错误码 */
    iretq                 /* 中断返回 */
.endm

# 有错误码的异常（#PF）——唯一区别是不压伪错误码，CPU 已经压了
.macro ISR_ERRCODE name vector handler
\name:
    /* 错误码已在栈上，直接保存寄存器 */
    pushq %rax
    /* ... 其余同上，恢复时 addq $8 跳过的是 CPU 压的错误码 ... */
    iretq
.endm

# 实例化两个 stub
ISR_NOERRCODE isr_bp_stub, 3, handle_bp
ISR_ERRCODE   isr_pf_stub, 14, handle_pf
```

这里的栈对齐是个很容易踩的坑。对于 #BP 这种没有硬件错误码的异常，CPU 只压入了 SS/RSP/RFLAGS/CS/RIP 五个值（5 × 8 = 40 字节），如果 ISR stub 直接保存寄存器的话，栈帧里的 error_code 字段就不存在了，C 处理函数读到的 `frame->error_code` 实际上是 CPU 压的 RIP——整个结构体全部错位。所以我们手动 `pushq $0` 填一个伪错误码，让 InterruptFrame 的布局保持统一。

还有一点需要特别注意：寄存器的保存和恢复顺序必须与 `InterruptFrame` 结构体中字段的声明顺序完全一致。我们的结构体从上到下是 r15→r14→...→rax，但压栈是反过来的（先 push rax 再 push r15），这样弹出的时候 `popq %r15` 刚好对应结构体的第一个字段——栈是从高地址往低地址增长的，先压的在栈顶（低地址），对应结构体的靠前成员。

**验证**：这一步没有独立的串口输出验证，但它是下一步的基础。

---

### 第四步——异常处理函数：打印寄存器 dump

**目标**：实现 `handle_bp` 和 `handle_pf` 两个 C 函数，通过 kprintf 输出完整的异常信息。

**代码**（`kernel/mini/arch/x86_64/exception_handlers.cpp`，关键部分）：

```cpp
extern "C" void handle_bp(InterruptFrame* frame) {
    dump_interrupt_frame(frame, "#BP", 3);
    kprintf("[EXCEPTION] Breakpoint triggered at RIP=0x%x\n", frame->rip);
    kprintf("[EXCEPTION] This is a software breakpoint, continuing...\n");
}

extern "C" void handle_pf(InterruptFrame* frame) {
    uint64_t fault_addr;
    __asm__ volatile ("movq %%cr2, %0" : "=r"(fault_addr));

    const char* present = (frame->error_code & 0x01) ? "protection" : "not present";
    const char* access  = (frame->error_code & 0x02) ? "write" : "read";

    dump_interrupt_frame(frame, "#PF", 14);
    kprintf("[EXCEPTION] Page Fault: %s on %s, CR2=0x%016x\n",
            present, access, fault_addr);
}
```

两个处理函数都是 `extern "C"` 的——这是因为 `interrupts.S` 里的 `call handle_bp` 是 C 链接约定，如果用 C++ 的 name mangling 的话链接器会找不到符号。这是内核开发中混合 C/C++/汇编时的常见模式。

`handle_pf` 里我们读取了 CR2 寄存器，这个寄存器是 x86 架构专门为页错误保留的——CPU 在触发 #PF 时会自动把导致缺页的线性地址写入 CR2，所以我们在处理函数里第一时间把它读出来。错误码的解析也很有用：bit 0 区分"页不存在"和"权限冲突"，bit 1 区分读还是写，这些信息在调试缺页问题的时候非常关键。

**验证**：这一步同样没有独立的串口输出，组合到 main.cpp 后一起验证。

---

### 第五步——整合到 main.cpp：用 int $3 点火测试

**目标**：在 mini_kernel_main 中依次调用 gdt_init → idt_init → pmm::init，然后触发 `int $3` 验证整个中断链路。

**代码**（`kernel/mini/main.cpp`，新增部分）：

```cpp
#include "arch/x86_64/gdt.hpp"
#include "arch/x86_64/idt.hpp"

extern "C" [[noreturn]] void mini_kernel_main(uint64_t boot_info_addr) {
    // ... 串口输出 BootInfo ...

    // GDT 必须在 IDT 之前！
    kprintf("[INIT] Setting up GDT...\n");
    cinux::mini::arch::gdt_init();
    kprintf("[INIT] GDT loaded successfully.\n");

    kprintf("[INIT] Setting up IDT...\n");
    cinux::mini::arch::idt_init();
    kprintf("[INIT] IDT loaded successfully.\n");

    // PMM 初始化
    cinux::mini::mm::pmm::init(boot_info);

    // 点火测试！
    kprintf("\n[TEST] Triggering breakpoint exception (int $3)...\n");
    __asm__ volatile("int $3");
    kprintf("[TEST] Breakpoint test passed! Execution continued after #BP.\n\n");

    while (1) { __asm__ volatile("cli; hlt"); }
}
```

初始化顺序很关键：GDT 必须在 IDT 之前，因为 IDT 条目引用了 GDT 中的代码段选择子。如果反过来，`lidt` 虽然能成功，但中断触发时 CPU 用 selector 去查 GDT 会查到垃圾数据，直接 triple fault。

`int $3` 是 x86 的断点指令（opcode 0xCC），它触发 #BP 异常。因为这个异常是陷阱类型（trap），所以 CPU 压入的 RIP 指向下一条指令——也就是说 `iretq` 返回后会继续执行后面的 `kprintf`，打印 "Breakpoint test passed!"。这正是我们想要验证的：触发异常不死机，能继续执行。

同时别忘了更新 CMakeLists.txt，把新增的源文件加进去：

```cmake
# kernel/mini/CMakeLists.txt 中添加
gdt.cpp
idt.cpp
interrupts.S
exception_handlers.cpp
```

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
Cinux Mini Kernel v0.1.0
BootInfo: entry_point=0xFFFFFFFF80020000, kernel_phys_base=0x20000
...
[INIT] Setting up GDT...
[INIT] GDT loaded successfully.
[INIT] Setting up IDT...
[INIT] IDT loaded successfully.
[MINI] PMM: Total 131040 pages (511 MB), Free 130784 pages (510 MB)

[TEST] Triggering breakpoint exception (int $3)...

==== EXCEPTION: #BP (vector 3) ====
  RIP   = 0xffffffff80020224   CS  = 0x0008
  RFLAGS= 0x0000000000000046
  RSP   = 0xffffffff80025100   SS  = 0x0010
  RAX=0x...  RBX=0x...  ...  R15=0x...
  ERROR CODE = 0x0000000000000000
========================================
[EXCEPTION] Breakpoint triggered at RIP=0xffffffff80020224
[EXCEPTION] This is a software breakpoint, continuing...
[TEST] Breakpoint test passed! Execution continued after #BP.
```

看到 `[TEST] Breakpoint test passed!` 就说明整个链路通了：GDT → IDT → ISR stub → C handler → iretq → 继续执行。

---

## 调试技巧

**Triple fault 直接重启，没有任何输出**

这是最常见的"灾难"。Triple fault 意味着在处理第一个异常的过程中又触发了异常（通常是 #GP），CPU 直接 reset。排查方法：用 `make run-debug` 启动 QEMU 调试模式，连 GDB 后在 `gdt_init` 和 `idt_init` 处打断点，单步跟踪。也可以检查 `s_gdt_pointer.base` 的值是否正确——在 higher-half kernel 里，GDT 地址需要是虚拟地址。

**`lidt` 之后一触发异常就 crash**

检查 IDT 条目中的 selector 是否与 GDT 一致。我们的代码里写死了 `SEGMENT_CODE64 = 0x08`，如果 GDT 的 code segment 不在索引 1，就会查到 null descriptor（全零，Present=0），触发 #GP。

**ISR stub 里寄存器保存顺序错乱**

对比 `InterruptFrame` 结构体的字段顺序和 `interrupts.S` 中的 `pushq` 顺序。记住：第一个 `pushq` 的值在栈顶（最低地址），对应结构体最后一个字段。我们的结构体最后三个字段是 rcx/rbx/rax，所以汇编里先 push rax，再 push rbx，最后 push rcx。

**用 GDB 验证 InterruptFrame**

```
(gdb) break handle_bp
(gdb) continue
# 触发 #BP 后断在 handle_bp
(gdb) print *frame
# 检查 frame->rip 是否指向 int $3 的下一条指令
(gdb) info registers
# 对比 frame 里的寄存器值和当前寄存器值
```

---

## 本章小结

| 组件 | 关键函数/结构 | 说明 |
|------|-------------|------|
| GDT | `GdtEntry`, `GdtPointer`, `gdt_init()` | 3 项 GDT（null/code64/data64），lgdt + far return 刷新 CS |
| IDT | `IdtEntry`, `InterruptFrame`, `idt_init()` | 256 项 IDT，配置 #BP(3) 和 #PF(14) |
| ISR Stub | `isr_bp_stub`, `isr_pf_stub` | 汇编宏，保存/恢复寄存器，调用 C handler |
| 异常处理 | `handle_bp()`, `handle_pf()` | 打印寄存器 dump，读取 CR2，解析错误码 |
| 关键寄存器 | GDTR, IDTR, CR2, CS/DS/SS | LGDT/LIDT 加载表地址，CR2 存缺页地址 |
| 关键指令 | `lgdt`, `lidt`, `lretq`, `iretq`, `int $3` | 表加载、段刷新、中断返回、软件断点 |

下一章（008_mini_kernel_disk_and_loader）我们会实现 ATA PIO 磁盘驱动和 ELF 加载器，让 mini kernel 能从磁盘加载大内核并跳转执行。届时 IDT 中已经配好的异常处理会继续发挥作用——大内核加载过程中的任何内存问题都能被 #PF 捕获并报告。
