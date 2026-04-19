# 009A Higher-Half 大内核启动基础设施 —— 让大内核在 0xFFFFFFFF80000000 "站起来"

## 章节导语

上一章（008）我们把 ATA 磁盘驱动和 ELF 加载器搞定了，mini kernel 已经有能力从磁盘读取一个大内核 ELF 文件、解析它的 PT_LOAD 段、把它们搬运到正确的物理地址，然后跳转到入口点执行。但问题在于——跳转到哪里？跳过去之后第一件事做什么？大内核的代码链接在什么样的地址上？这些问题在 008 里我们一笔带过了，因为那时候大内核本身还不存在。

这一章我们要做的事情就是：**让大内核真正"站起来"**。具体来说，我们需要编写大内核自己的链接脚本（linker script），把代码链接到 higher-half 虚拟地址 `0xFFFFFFFF80000000`；编写一段启动汇编（boot.S），负责在 mini kernel 跳过来之后的第一时间设置栈、清零 BSS、运行全局构造器、最后跳进 C++ 世界；再写一组 C++ 运行时桩函数（crt_stub），把编译器在 freestanding 环境下期望的那些符号全部补齐。完成本章后，你会看到 QEMU 串口输出 `[BIG] Big kernel running @ 0x1000000`——这意味着大内核的第一行代码已经成功执行了。

本章的前置知识是上一章（008_mini_kernel_disk_and_loader）的磁盘驱动和 ELF 加载器，因为大内核是被 mini kernel 从磁盘加载并跳转过来的。如果你还没读完 008，建议先回去补完。

---

## 概念精讲

### 为什么内核要住在 higher-half？

你可能会问：我们的 mini kernel 不是一直在物理地址的 identity mapping 下跑得好好的吗？为什么大内核非得搬到 `0xFFFFFFFF80000000` 这么远的地址上去？

原因有好几个。最直接的一个是**用户空间的地址空间规划**。在现代操作系统中，内核和用户进程共享同一个虚拟地址空间。如果我们把内核放在低地址（比如 0x0 到 0x100000 这片区域），用户进程的代码就没法用这些地址了。而把内核放到虚拟地址空间的最高端（x86-64 的 canonical high 区域），低地址就完全留给用户进程——每个进程都能拥有一整块连续的地址空间，内核映射在所有进程的页表里始终存在，系统调用的时候不需要切换页表，只需要修改 CS:RIP 跳到高地址就行了。

另一个原因是**安全性**。内核代码和数据不应该被用户态程序随意访问。Higher-half 的布局天然地把内核地址和用户地址分开了——只要在页表项里把用户/内核位（U/S bit）设对，用户态的代码根本不可能碰触到内核空间。如果把内核和用户程序混在同一个地址范围里，保护起来就要麻烦得多。

所以几乎所有现代 OS（Linux、Windows、macOS）都采用 higher-half 内核的设计。x86-64 的 48 位虚拟地址空间里，高半部分（`0xFFFF800000000000` 到 `0xFFFFFFFFFFFFFFFF`）属于内核，低半部分属于用户进程。我们选择的 `0xFFFFFFFF80000000` 是一个常见的基址选择——Linux 内核也用这个地址作为 `__START_KERNEL_map`。

```
x86-64 虚拟地址空间布局 (48-bit canonical):

0x0000000000000000 ┌──────────────────┐
                  │                  │
                  │   用户空间         │  低半部分，每个进程独立
                  │   (128 TB)        │
                  │                  │
0x00007FFFFFFFFFFF ├──────────────────┤ ← canonical boundary
                  │  不可访问 (hole)   │  非 canonical 地址，访问触发 #GP
0xFFFF800000000000 ├──────────────────┤
                  │                  │
                  │   内核空间         │  高半部分，所有进程共享
                  │   (128 TB)        │
                  │                  │
0xFFFFFFFF80000000 │  ← 我们的 KERNEL_VMA
                  │  大内核从这里开始   │
0xFFFFFFFFFFFFFFFF └──────────────────┘
```

### VMA 和 LMA 到底是什么？

在链接脚本里你会频繁看到两个概念：VMA（Virtual Memory Address）和 LMA（Load Memory Address）。理解这两个概念是搞懂 higher-half 内核链接过程的关键。

VMA 是"链接器认为这个代码/数据在运行时应该出现在哪个虚拟地址"。编译器生成相对跳转、绝对地址引用、函数指针这些的时候，用的都是 VMA。比如大内核的 `kernel_main` 函数，链接器会把它安排在 `0xFFFFFFFF80000000` 之上的某个 VMA，所有引用这个函数地址的代码都会使用这个高地址。

LMA 是"这个代码/数据在加载时实际应该被放到哪个物理地址"。我们的 ELF 加载器在加载大内核的时候，页表还是 mini kernel 设置的 identity mapping，CPU 还不认识 `0xFFFFFFFF80000000` 这个虚拟地址——所以段数据必须被加载到一个物理地址（也就是 LMA），代码才能在 identity mapping 下正常执行。

在链接脚本里，`. = KERNEL_VMA + KERNEL_LMA` 这行代码设置了"位置计数器"的初始值。这个值既不是纯 VMA 也不是纯 LMA，而是一个巧妙的偏移。之后每个 section 的 VMA 会从这个位置计数器开始递增，而 `AT(ADDR(.text) - KERNEL_VMA)` 伪操作把 LMA 设置为 VMA 减去 `KERNEL_VMA`，也就是把虚拟地址映射回物理地址。举个例子：如果 `.text` 的 VMA 是 `0xFFFFFFFF80101000`（VMA base + 0x101000），那么 `ADDR(.text) - KERNEL_VMA = 0x101000`，这个值就是 LMA——也就是说这段代码会被加载到物理地址 `0x101000`，在 identity mapping 下 CPU 能直接执行。

```
链接脚本地址关系:

VMA (链接器看到的地址):        LMA (加载器放到内存的地址):
0xFFFFFFFF80000000 ─┐          0x01000000 ─┐
  .text.start       │ -0xFFFFFFFF80000000   │
  .text             │  ──────────────────→   │ 物理内存中的实际位置
  .rodata           │  AT() 做的减法         │
  .data             │                        │
  .init_array       │                        │
  .bss              │                        │
  stack             │                        │
0xFFFFFFFF80XXXXXX ─┘          0x01XXXXXX ─┘

关键公式: LMA = VMA - KERNEL_VMA
```

### .init_array 和全局构造器：C++ 内核的隐藏需求

如果你写的是 C 内核，启动汇编只需要设置栈、清 BSS、调用 main 就行了。但 C++ 有一套额外的机制：**全局对象的构造函数必须在 main 之前被调用**。编译器处理全局对象（比如 `static std::string foo = "bar"`）的方式是，生成一个构造器函数指针，把它放进 `.init_array` section 里。然后在 main 之前，启动代码需要遍历这个数组，逐个调用这些构造器。

这就是链接脚本里 `.init_array` section 和 `__init_array_start/end` 两个边界符号的作用。`_init_global_ctors()` 函数（在 crt_stub.cpp 里）拿到这两个符号的地址，算出数组的大小，然后 for 循环调用每一个函数指针。如果我们不做这一步，所有带非平凡构造函数的全局对象都会处于未初始化状态——里面的虚函数表指针是垃圾数据，成员变量是随机值，调用它们的任何方法都是未定义行为。当然，我们当前的内核还没有这样的全局对象，但基础设施必须提前搭好，不然后续加上带构造函数的全局变量时会踩一个极其隐蔽的坑。

---

## 动手实现

### 第一步——编写大内核链接脚本

**目标**：定义大内核的内存布局，把代码链接到 higher-half 虚拟地址空间，同时确保 ELF 的程序头包含正确的物理地址信息供 mini kernel 的 ELF 加载器使用。

**代码**（文件路径：`kernel/linker.ld`）：

```ld
/* ==============================================================
 * Cinux Big Kernel - Linker Script
 * ============================================================== */

OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

KERNEL_VMA   = 0xFFFFFFFF80000000;   /* higher-half virtual base */
KERNEL_LMA   = 0x1000000;            /* physical load address (16 MB) */

SECTIONS
{
    . = KERNEL_VMA + KERNEL_LMA;

    .text : AT(ADDR(.text) - KERNEL_VMA) ALIGN(4096) {
        *(.text.start)         /* _start MUST be first */
        *(.text .text.*)
        *(.rodata .rodata.*)
    }

    .data : AT(ADDR(.data) - KERNEL_VMA) ALIGN(4096) {
        *(.data .data.*)
    }

    .init_array : AT(ADDR(.init_array) - KERNEL_VMA) ALIGN(8) {
        __init_array_start = .;
        KEEP(*(.init_array .init_array.*))
        __init_array_end   = .;
    }

    .bss : ALIGN(4096) {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    }

    __kernel_end = .;
    PROVIDE(__kernel_size = __kernel_end - (KERNEL_VMA + KERNEL_LMA));

    .stack (NOLOAD) : ALIGN(4096) {
        . = . + 0x4000;          /* 16 KB stack */
        __kernel_stack_top = .;
    }

    /DISCARD/ : {
        *(.comment*)
        *(.note*)
        *(.eh_frame*)
    }
}
```

**解释**：

先来看开头几个伪操作。`OUTPUT_FORMAT("elf64-x86-64")` 和 `OUTPUT_ARCH(i386:x86-64)` 告诉链接器我们要生成 x86-64 的 ELF 文件。`ENTRY(_start)` 指定入口点符号——这个符号定义在 boot.S 里，是 mini kernel 跳转过来时执行的第一条指令的位置。

接下来是两个地址常量。`KERNEL_VMA = 0xFFFFFFFF80000000` 是 higher-half 的虚拟基地址，所有代码和数据的 VMA 都会从这个地址往上排布。`KERNEL_LMA = 0x1000000`（16MB）是物理加载地址——这也是 mini kernel 的 ELF 加载器把大内核搬运到的物理内存位置。这个值要和 mini kernel 的 `BIG_KERNEL_LOAD_ADDR` 常量一致，不然加载器搬过去的段对不上链接器期望的物理地址，整个内核就是一团乱。

SECTIONS 块里最关键的一行是 `. = KERNEL_VMA + KERNEL_LMA`。这行把位置计数器设为 `0xFFFFFFFF80000000 + 0x1000000 = 0xFFFFFFFF80100000`。之后所有 section 的 VMA 就会从 `0xFFFFFFFF80100000` 开始递增。但光设 VMA 还不够——如果链接器把 LMA 也设成 VMA 那个天文数字，ELF 加载器就要往 `0xFFFFFFFF80100000` 写数据，但我们的页表还没建立起来，CPU 连 `0xFFFFFFFF80000000` 都不认识。所以每个 section 都用 `AT()` 伪操作显式指定了 LMA：`AT(ADDR(.text) - KERNEL_VMA)` 把 VMA 减去 `0xFFFFFFFF80000000`，得到的就是纯物理偏移量。这样一来，ELF 文件的程序头（program header）里 `p_vaddr` 是高地址（供将来开启分页后使用），`p_paddr` 是低地址（供当前 identity mapping 使用），两边都不耽误。

Section 的排列顺序是有讲究的。`.text.start` 必须放在 `.text` 的最前面——我们用 `*(.text.start)` 匹配 boot.S 里用 `.section .text.start` 定义的代码，确保 `_start` 函数出现在 ELF 文件的最开头。这也是 mini kernel 跳转到入口点时执行的第一段代码。`.text` 和 `.rodata`（只读数据）合并在一起，放在同一个 PT_LOAD 段里。`.data`（可写数据）单独一个段。`.init_array` 放全局构造器指针，用 `KEEP()` 防止链接器的垃圾回收（`--gc-sections`）把这些看似"没人引用"的函数指针删掉——它们是被 `_init_global_ctors` 通过 linker script 符号间接引用的，链接器的静态分析发现不了这个依赖。`.bss` 不需要 `AT()` 因为它不占文件空间——`memsz > filesz` 的部分由启动汇编负责清零。

最后是栈的分配。`.stack (NOLOAD)` 表示这个 section 不占用 ELF 文件空间（不产生文件内容），只是在 VMA 空间里预留了 16KB（`0x4000`）。`__kernel_stack_top` 指向栈顶，boot.S 里 `movq $__kernel_stack_top, %rsp` 就是把这个地址加载到栈指针寄存器。x86-64 的栈是向下增长的，所以"栈顶"其实是这片内存的最高地址。

---

### 第二步——编写大内核启动汇编 boot.S

**目标**：实现 `_start` 函数，这是大内核执行的第一段代码。它需要依次完成：禁用中断、设置栈、清零 BSS、运行全局构造器、调用 kernel_main、以及一个防止 kernel_main 返回的死循环。

**代码**（文件路径：`kernel/arch/x86_64/boot.S`）：

```asm
.section .text.start, "ax"
.code64

.global _start
.type   _start, @function

_start:
    /* Step 1: Disable interrupts */
    cli                           # disable interrupts

    /* Step 2: Set up the kernel stack */
    movq  $__kernel_stack_top, %rsp  # rsp→stack top: set stack pointer
    xorq  %rbp, %rbp                # 0→rbp: clear base pointer (end of call chain)

    /* Step 3: Clear BSS section */
    movq  $__bss_start, %rdi       # &bss_start→rdi: destination address
    movq  $__bss_end, %rcx         # &bss_end→rcx: end address
    subq  %rdi, %rcx               # end-start→rcx: byte count
    xorq  %rax, %rax               # 0→rax: fill byte
    rep stosb                      # rax→[rdi]: zero-fill BSS, rcx times

    /* Step 4: Run global constructors (.init_array) */
    call  _init_global_ctors

    /* Step 5: Call the C++ main function */
    xorq  %rdi, %rdi              # NULL→rdi: BootInfo* (unused for now)
    call  kernel_main

    /* Step 6: Halt (kernel_main should never return) */
.halt:
    cli                           # disable interrupts
    hlt                           # halt processor
    jmp   .halt                   # loop→.halt: in case of NMI, re-halt

.size _start, . - _start
```

**解释**：

我们逐个步骤来拆解。

Step 1 是 `cli`，一条指令禁用中断。为什么要先做这个？因为大内核目前还没有设置自己的 IDT（中断描述符表），如果这时候中断是开启的，任何一个硬件中断（定时器、键盘中断、哪怕是一个 IPI）都会触发 CPU 去查 IDT——但 IDT 里装的还是 mini kernel 的中断处理函数地址。mini kernel 的代码和数据在跳转到大内核之后可能还在内存里（我们没覆盖它），但它的栈和状态已经不适用了，执行它的中断处理函数大概率会崩溃。所以上来先把中断关掉，等后续章节大内核建立起自己的 IDT 之后再重新开启。

Step 2 设置栈。`movq $__kernel_stack_top, %rsp` 把链接脚本里定义的 `__kernel_stack_top` 的地址加载到 RSP。注意 AT&T 语法里 `$__kernel_stack_top` 是"取这个符号的值"——也就是 16KB 栈空间的最高地址。`xorq %rbp, %rbp` 把基址指针清零，这样栈回溯（stack unwinding）工具就知道这是调用链的最底部——没有更上一层调用者了。这两个操作合在一起，等于告诉 CPU"栈从这里开始往下用"。

Step 3 是清零 BSS，这一步非常关键。BSS 段存放的是所有未初始化的全局变量和静态变量。按 C/C++ 语义，这些变量在程序启动时必须为零。理论上，ELF 加载器在加载 PT_LOAD 段的时候，如果 `memsz > filesz`，应该把多出来的部分清零——这就是 BSS。但问题是我们的 ELF 加载器的实现可能没有完美处理这个边界情况，特别是当 BSS 恰好在段的末尾、文件对齐导致 `filesz` 不精确的时候。所以我们不依赖加载器，在启动汇编里无条件清一遍 BSS，保证万无一失。

清零 BSS 的具体手法是用 `rep stosb` 指令。先把 `__bss_start` 的地址放进 `%rdi`（目标地址），`__bss_end` 放进 `%rcx`（结束地址），`subq %rdi, %rcx` 算出字节数，`xorq %rax, %rax` 把 `%rax` 清零作为填充值，然后 `rep stosb` 就会自动执行 `%rcx` 次字节写入操作——每次把 `%al`（即 0）写到 `%rdi` 指向的位置，然后 `%rdi` 自增、`%rcx` 自减。这是一条经典的 x86 字符串操作指令，用来做内存填充非常高效。

⚠️ 注意：`rep stosb` 会破坏 `%rdi`、`%rcx`、`%rax` 的值。这是后续 Step 5 之前我们需要知道的事情——在清零 BSS 之后，这三个寄存器的值已经不再可靠了。

Step 4 调用 `_init_global_ctors()`，这个函数定义在 crt_stub.cpp 里，负责遍历 `.init_array` section 中的函数指针数组并逐个调用。这就是前面概念精讲里提到的全局构造器机制。在我们当前的大内核里，还没有带非平凡构造函数的全局对象，所以 `.init_array` 是空的，这个函数调用实际上什么也不做。但基础设施先搭好，后面写 `static Logger g_logger;` 这种全局对象的时候就不会踩坑了。

Step 5 调用 `kernel_main()`，终于进入 C++ 世界了。这里有一个细节：`xorq %rdi, %rdi` 把 `%rdi`（第一个参数寄存器）清零了，传给 `kernel_main` 的参数是 `nullptr`（即 `BootInfo* = NULL`）。这是因为我们在清零 BSS 的时候破坏了 `%rdi` 的原始值——mini kernel 在跳转时原本通过 `%rdi` 传了一个 BootInfo 指针，但经过 BSS 清零之后这个值已经丢了。代码里的注释也标注了这个 TODO：后续如果需要 BootInfo，必须在 BSS 清零之前把 `%rdi` 保存起来（比如压栈或者存到另一个寄存器），清零之后再恢复。目前 milestone 009 不需要 BootInfo，所以先传 NULL。

Step 6 是一个三指令的死循环：`cli` 关中断、`hlt` 停机、`jmp .halt` 跳回开头。为什么要写三行而不是一行 `hlt`？因为 `hlt` 指令在收到 NMI（Non-Maskable Interrupt，不可屏蔽中断）或者 SMY（System Management Interrupt）的时候会被唤醒。如果在 `hlt` 之后 CPU 被 NMI 唤醒了，没有 `jmp .halt` 的话它就会继续往下执行——但后面什么代码都没有，CPU 会从内存中读取垃圾数据当指令执行，这通常会导致 triple fault 和重启。`jmp .halt` 保证了即使被 NMI 打断，CPU 也会立刻重新关中断并停机。

---

### 第三步——实现 C++ 运行时桩函数 crt_stub

**目标**：提供 freestanding C++ 环境下编译器期望的一系列运行时函数，包括纯虚函数调用处理、栈保护失败处理、atexit 注册、全局构造器初始化、以及 operator new/delete 的 stub。

**代码**（文件路径：`kernel/arch/x86_64/crt_stub.cpp`，纯虚函数与栈保护部分）：

```cpp
#include <stdint.h>

extern "C" {

[[noreturn]] void __cxa_pure_virtual() {
    __asm__ volatile("cli");
    __asm__ volatile("outb %0, %1" : : "a"((uint8_t)'V'), "Nd"((uint16_t)0xE9));
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}

[[noreturn]] void __stack_chk_fail() {
    __asm__ volatile("cli");
    __asm__ volatile("outb %0, %1" : : "a"((uint8_t)'S'), "Nd"((uint16_t)0xE9));
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}

int __cxa_atexit(void (*)(void*), void*, void*) {
    return 0;
}
```

**解释**：

这几个函数代表的是"不应该发生但编译器需要链接"的场景。

`__cxa_pure_virtual` 是 C++ ABI 规定的符号——当程序试图调用一个纯虚函数时，链接器需要这个符号。什么时候会触发它？最常见的场景是：在基类构造函数里调用了虚函数。C++ 对象构造的时候，虚函数表（vtable）指针是逐级设置的——基类构造期间，vtable 指向基类的虚函数表，如果基类有纯虚函数，这时调用它就会走进 `__cxa_pure_virtual`。在我们这种 freestanding 环境里，这种情况不太常见，但只要用了继承和虚函数，编译器就会要求这个符号存在。我们让它在 QEMU 的 debug console（I/O 端口 0xE9）输出字符 `'V'`，然后死循环。如果你在 `debug.log` 里看到一个 `'V'`，就知道是纯虚函数被调用了。

`__stack_chk_fail` 是栈保护（stack canary）失败时的回调。编译器在函数 prologue 里往栈上放一个随机值（canary），函数返回之前检查这个值是否被改过——如果被改了，说明发生了栈缓冲区溢出。不过我们的编译选项里有 `-fno-stack-protector`，所以编译器根本不会插入 canary 检查代码，这个函数理论上永远不会被调用。但提供它没有坏处——万一以后有人改了编译选项忘了改回来，至少能 halt 而不是链接失败。

`__cxa_atexit` 是 `atexit` 机制的底层实现。编译器用它来注册需要在程序退出时调用的清理函数——比如全局对象的析构函数。但内核永远不会"退出"，所以这个函数直接返回 0（成功），什么都不做。析构函数永远不会被调用，对于内核来说完全没问题——机器关机的时候谁在乎那几个对象有没有被正确析构呢？

**代码**（文件路径：`kernel/arch/x86_64/crt_stub.cpp`，全局构造器部分）：

```cpp
extern void (*__init_array_start[])();
extern void (*__init_array_end[])();

void _init_global_ctors() {
    void (**start)() = __init_array_start;
    void (**end)()   = __init_array_end;

    for (void (**func)() = start; func != end; func++) {
        void (*ctor)() = *func;
        if (ctor != nullptr) {
            ctor();
        }
    }
}

}  // extern "C"
```

这段代码的核心逻辑就是遍历 `__init_array_start` 到 `__init_array_end` 之间的函数指针数组，逐个调用。`__init_array_start` 和 `__init_array_end` 是链接脚本里定义的符号——它们标记了 `.init_array` section 的起始和结束位置。把这两个符号声明为函数指针数组（`void (*)()[]`）是 C/C++ 里处理 linker script 符号的标准手法：链接器给这两个符号分配的"值"就是它们的地址本身，所以声明成数组之后，`start` 和 `end` 就分别指向数组的首元素和尾后元素，`end - start` 就是元素个数。

注意循环体里有一个 `if (ctor != nullptr)` 检查。这是因为 `.init_array` section 可能有对齐填充导致的空隙——某些编译器/链接器组合下，数组里可能出现 NULL 指针。调用 NULL 函数指针是未定义行为（在内核里大概率 triple fault），所以必须跳过。

**代码**（文件路径：`kernel/arch/x86_64/crt_stub.cpp`，operator new/delete 部分）：

```cpp
// Must be outside extern "C" -- they need C++ mangling.

void* operator new(unsigned long size) {
    (void)size;
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}

void* operator new[](unsigned long size) {
    (void)size;
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}

void operator delete(void* ptr) noexcept {
    (void)ptr;
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}

void operator delete(void* ptr, unsigned long size) noexcept {
    (void)ptr;
    (void)size;
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}
```

这些 operator new/delete 的实现方式可能看起来有点极端——直接死循环。但这是有意的。我们的大内核目前没有堆分配器（heap allocator），任何动态内存分配的需求（`new`、`new[]`）都代表一个编程错误。死循环而不是 `return nullptr` 的好处是：如果你不小心用了 `new`，CPU 会立刻卡住，你很快就能在 GDB 里发现是哪里卡了。如果返回 `nullptr`，问题可能要等到后面解引用空指针的时候才暴露，排查起来就麻烦多了。这些函数必须放在 `extern "C"` 块的外面，因为 `operator new` 需要 C++ 的 name mangling——编译器生成的调用指令用的是 mangled name，如果用 C 链接约定，链接器就找不到了。

---

### 第四步——编写大内核 C++ 入口点

**目标**：实现 `kernel_main()` 函数，初始化串口并打印里程碑消息。

**代码**（文件路径：`kernel/main.cpp`）：

```cpp
#include <stdint.h>
#include "kernel/lib/kprintf.hpp"

extern "C" void kernel_main() {
    // Step 1: Initialise the serial port used by kprintf
    cinux::lib::kprintf_init();

    // Step 2: Print the milestone message
    cinux::lib::kprintf("[BIG] Big kernel running @ 0x1000000\n");

    // Halt
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}
```

**解释**：

`kernel_main` 是整个大内核的 C++ 入口点，被 boot.S 里的 `call kernel_main` 调用。函数做的事情很直白：先调用 `kprintf_init()` 初始化串口（COM1，115200 8N1），然后打印一行里程碑消息确认我们到达了这里，最后进入死循环。

关于 `kprintf`，大内核使用的是 `kernel/lib/kprintf.hpp`（注意命名空间是 `cinux::lib`），和 mini kernel 使用的 `kernel/mini/lib/kprintf.h`（命名空间 `cinux::mini::lib`）是两套独立的实现。大内核的 kprintf 内部持有一个 `Serial` 类的静态实例（`static Serial g_serial(SERIAL_COM1)`），`kprintf_init()` 调用 `g_serial.init()` 来配置 UART 硬件。之所以要重新初始化串口，是因为 mini kernel 虽然也初始化过串口，但在大内核启动的时候我们不能假设任何硬件状态——也许将来大内核会用不同的串口配置，也许 mini kernel 在跳转之前做了一些操作影响了串口状态。重新初始化一次是最安全的做法。

最后那个 `while (1) { cli; hlt; }` 循环是 kernel_main 的"正常退出"——虽然函数签名没有 `[[noreturn]]` 属性，但实际上这个循环永远不会结束。boot.S 的 `.halt` 标签也有一个类似的死循环，双重保险：即使 kernel_main 的这个循环不知怎么被跳过了，boot.S 的 `.halt` 也会兜底。

---

### 第五步——理解 mini kernel 如何跳转到大内核

虽然这一步不需要我们写新代码，但理解跳转机制是整个启动链的最后一环。我们回头看 mini kernel 的 `main.cpp` 里，加载完大内核之后的关键代码。

**代码**（文件路径：`kernel/mini/main.cpp`，跳转部分）：

```cpp
uint64_t entry = cinux::mini::loader::load_big_kernel(
    cinux::mini::loader::BIG_KERNEL_LBA);
if (entry == 0) {
    kprintf("[MINI] ERROR: Failed to load big kernel!\n");
    while (1) __asm__ volatile("cli; hlt");
}

kprintf("[MINI] Jumping to big kernel at 0x%p...\n", entry);

__asm__ volatile(
    "cli            \n\t"  // disable interrupts before handoff
    "jmp *%0        \n\t"
    :
    : "r"(entry)
    : "memory");
```

`load_big_kernel()` 返回的是一个物理地址——ELF 入口点 `e_entry` 减去 `0xFFFFFFFF80000000` 之后的值。对于我们的大内核来说，`_start` 的 VMA 大约在 `0xFFFFFFFF80100000`，所以物理入口大约在 `0x100000`（16MB 附近）。mini kernel 拿到这个物理地址后，用 `jmp *%0` 做间接跳转——`"r"(entry)` 让编译器把 entry 值放进一个通用寄存器，`jmp *%0` 就是跳转到那个寄存器指向的地址。跳转之前先 `cli` 关中断，原因和我们 boot.S 里的 Step 1 一样——大内核还没有设置自己的 IDT。

这个跳转完成之后，CPU 的执行流就从 mini kernel 切换到了大内核的 `_start`，之后就是 boot.S 的六个步骤依次执行了。整个启动链条至此完整：BIOS → MBR → Stage2 → mini kernel → big kernel，一条线串下来。

---

## 构建与运行

```bash
# 从项目根目录
git checkout 009_big_kernel_boot
cmake -B build -DCMAKE_BUILD_TYPE=Debug -S .
cmake --build build -j$(nproc)
cd build && make run
```

**期望输出**：

```
Cinux Mini Kernel v0.1.0
BootInfo: entry_point=0x..., kernel_phys_base=0x20000
...
[INIT] Setting up GDT...
[INIT] GDT loaded successfully.
[INIT] Setting up IDT...
[INIT] IDT loaded successfully.
[MINI] PMM: Total ... pages (... MB), Free ... pages (... MB)
...
[INIT] Initializing ATA controller...
[INIT] ATA controller initialized successfully (status=0x50).
...
[LOADER] Phase 1: Reading ... sectors from LBA 0x350...
[LOADER] ELF file: ... bytes (... sectors)
[LOADER] Mapping physical memory up to 0x...
=== Memory Layout ===
  Page Tables: 0x... - 0x... (... KB)
  Mini Kernel: 0x... - 0x... (... KB)
  PT_LOAD target: 0x... - 0x... (... KB)
  [OK] No overlaps detected.
=====================
[LOADER] Phase 2: Reading ... sectors from disk...
[LOADER] Big kernel loaded successfully.
[LOADER] Entry point: 0x...
[MINI] Jumping to big kernel at 0x...

[BIG] Big kernel running @ 0x1000000
```

看到最后一行 `[BIG] Big kernel running @ 0x1000000` 就说明大内核已经成功启动了。从 mini kernel 的 `Jumping to big kernel` 到大内核的 `[BIG]` 输出，中间经历了 boot.S 的全部六个步骤：关中断、设栈、清 BSS、跑全局构造器、调用 kernel_main、初始化串口、打印消息。整条链路走通，没有任何 triple fault 或挂死。

---

## 调试技巧

**大内核没有任何输出，QEMU 直接重启**

这是最常见的灾难场景。多半是 boot.S 里的某个步骤搞砸了导致 triple fault。用 `make run-debug` 启动 GDB，在 `0x1000000`（或者你的实际入口物理地址）设断点：`break *0x1000000`，然后 `continue`。如果断点命中了但 `x/10i $rip` 看到的指令不像你的 boot.S 代码，说明入口地址不对——检查链接脚本和 ELF 加载器的 higher-half 地址转换逻辑。如果断点根本没命中，说明 mini kernel 的跳转本身就出了问题——可能是 `load_big_kernel` 返回了错误的入口地址。

**BSS 清零把不该清的数据清了**

这种 bug 非常阴险，症状是某些全局变量的值莫名其妙变成了 0。用 GDB 检查 `__bss_start` 和 `__bss_end` 的值是否合理：`print (void*)__bss_start` 和 `print (void*)__bss_end`。如果 `__bss_end` 比 `__bss_start` 小，说明链接脚本的 section 排列有问题——位置计数器没有递增反而递减了，`rep stosb` 会试图清零一个天文数字大小的区域，大概率会覆盖掉 BSS 之前的数据段甚至代码段。

**用 GDB 单步跟踪启动流程**

```bash
# 终端 1: 启动 QEMU 调试模式
cd build && make run-debug

# 终端 2: 启动 GDB
gdb build/kernel/mini/mini_kernel
(gdb) target remote :1234
(gdb) break *0x1000000       # 在大内核入口设断点
(gdb) continue
# 断点命中后
(gdb) x/20i $rip             # 查看即将执行的指令
(gdb) si                     # 单步执行（进入函数）
(gdb) print $rsp             # 检查栈指针
(gdb) info registers          # 查看所有寄存器状态
```

如果你想在 `_init_global_ctors` 处设断点，用 `break _init_global_ctors`。在 `kernel_main` 处同理。如果 GDB 提示找不到符号，检查调试符号文件路径是否正确——大内核的调试符号在 `build/kernel/` 目录下（不是 `build/kernel/mini/`）。

---

## 本章小结

| 组件 | 关键符号/函数 | 说明 |
|------|-------------|------|
| 链接脚本 | `KERNEL_VMA`, `KERNEL_LMA` | higher-half 虚拟基址和物理加载地址 |
| 链接脚本 | `AT(ADDR(.text) - KERNEL_VMA)` | 把 VMA 映射回物理 LMA |
| 链接脚本 | `__init_array_start/end` | 全局构造器数组边界 |
| 链接脚本 | `__bss_start/end` | BSS 段边界，供清零使用 |
| 链接脚本 | `__kernel_stack_top` | 16KB 内核栈顶 |
| boot.S | `_start` | 大内核入口，6 步启动序列 |
| boot.S | `cli` → `movq %rsp` → `rep stosb` → `call _init_global_ctors` → `call kernel_main` → `.halt` | 启动流程 |
| crt_stub | `__cxa_pure_virtual` | 纯虚函数调用处理，halt + 输出 'V' |
| crt_stub | `__stack_chk_fail` | 栈保护失败处理，halt + 输出 'S' |
| crt_stub | `__cxa_atexit` | atexit 注册，no-op（内核永不退出） |
| crt_stub | `_init_global_ctors` | 遍历 .init_array 调用全局构造器 |
| crt_stub | `operator new/delete` | 无堆分配器，调用直接 halt |
| main.cpp | `kernel_main` | C++ 入口，初始化串口 + 打印里程碑消息 |

下一章（009B）我们会在大内核里搭建 I/O 端口访问层和串口驱动——这次是从大内核的视角重新实现，和 mini kernel 的版本有所不同。有了 I/O 基础设施之后，大内核就能和外部硬件设备通信了，这是后续实现键盘驱动、VGA 文本模式输出、乃至真正的内存管理的前提。
