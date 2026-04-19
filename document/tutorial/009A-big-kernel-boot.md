# Higher-Half 大内核启动：从 Linker Script 到第一行 C++ 输出的完整实录

> 作者：
> 标签：higher-half, linker-script, boot.S, ELF, freestanding C++, x86-64, init_array, BSS, 内核开发, 裸机

---

## 前言

说实话，搞到 milestone 009 的时候，我们一直在 mini kernel 那个"舒适区"里打转。mini kernel 能启动、能管理物理内存、能处理中断、能从磁盘读 ELF 文件——看起来很厉害了对吧？但有一个让人始终不太舒服的事实：mini kernel 本质上是一个 flat binary，它没有链接脚本定义段布局，没有 proper 的 BSS 清零机制（全靠启动汇编里硬编码地址），甚至连"内核运行在虚拟地址空间的哪里"这个问题都不存在——因为它跑在物理地址上，identity mapping 一把梭。

这种做法在小内核阶段完全可以接受，甚至可以说是最简单的选择。但当我们开始构建真正的"大内核"——那个未来会包含文件系统、进程管理、网络协议栈的完整 OS 内核——就需要一套更正规的启动基础设施了。大内核需要一个像样的链接脚本，告诉链接器每个段应该放在虚拟地址空间的哪个位置；需要一个规范的启动汇编流程，从 mini kernel 手里接过接力棒后，依次完成设栈、清 BSS、调用全局构造函数这些脏活；需要一个 freestanding C++ 运行时的最小桩代码集，让编译器不抱怨缺符号；最后还需要一个 C++ 入口函数，证明这一整条启动链确实走通了。

这一章我们要做的就是这个"从零搭建大内核启动基础设施"的完整过程。核心产出是四个文件：`kernel/linker.ld` 定义 higher-half 内存布局，`kernel/arch/x86_64/boot.S` 做底层初始化，`kernel/arch/x86_64/crt_stub.cpp` 提供 freestanding 桩函数，`kernel/main.cpp` 作为 C++ 入口点。最终验证标准很简单：QEMU 串口输出 `[BIG] Big kernel running @ 0x1000000` 这一行字。你别看就一行输出，它背后是整个启动链条从 MBR 到 Stage2 到 mini kernel 到大内核的完整接力——任何一个环节出问题，这行字都不会出现。

## 环境说明

实验环境和之前一样：x86_64 平台，GNU AS + GCC/G++ + CMake 构建，QEMU 模拟运行。大内核是 freestanding C++23，编译标志和 mini kernel 略有不同——用的是 `-mcmodel=kernel` 而不是 mini kernel 的 `-mcmodel=large`，因为大内核被设计为运行在 higher-half 的"负 2GB"范围内，`-mcmodel=kernel` 可以让编译器用 32 位相对地址引用内核内部符号，生成更紧凑的代码。其余的编译约束不变：无标准库（`-ffreestanding -nostdlib`）、无异常（`-fno-exceptions`）、无 RTTI（`-fno-rtti`）、无栈保护（`-fno-stack-protector`）、无红区（`-mno-red-zone`）。

内存布局方面需要提前交代几个关键参数。大内核的物理加载地址（LMA）是 `0x1000000`（16MB），和 mini kernel 的 `0x20000` 之间有将近 16MB 的间距，互不干扰。虚拟基址（VMA）是 `0xFFFFFFFF80000000`——这是 x86-64 higher-half 内核的经典位置，和 Linux 的 `__START_KERNEL_map` 一致。不过有一个微妙的地方需要注意：当前 milestone 中，大内核实际上运行在 identity mapping 的物理地址空间中（因为 mini kernel 只建立了 identity mapping），链接脚本定义的 higher-half 地址要到后续 milestone 建立了 proper 的页表映射之后才能真正使用。这个设计选择后面会详细解释。

## 第一步——Linker Script：给内核一个确定的家

链接脚本 `kernel/linker.ld` 是整个大内核基础设施的基石。它决定了两件最根本的事情：内核在虚拟地址空间中的位置，以及每个段（.text、.data、.bss 等）怎么排列。理解了这个文件，后面 boot.S 和 crt_stub.cpp 中出现的所有 linker symbol——`__bss_start`、`__bss_end`、`__kernel_stack_top`、`__init_array_start`——就都有了着落。

脚本开头三行声明了输出格式和架构：

```ld
OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)
```

`OUTPUT_FORMAT` 告诉链接器我们生成的是 x86-64 的 ELF64 文件，`OUTPUT_ARCH` 指定目标架构，`ENTRY(_start)` 声明程序入口。`_start` 必须是 ELF 文件的第一个字节，这一点非常关键——mini kernel 的 ELF loader 解析 ELF 头中的 `e_entry` 得到入口虚拟地址，转换成物理地址后跳转过去。如果 `_start` 不是 ELF 文件的第一个字节，跳转就会落空。

接下来是两个地址常量，它们是理解整个链接脚本的关键：

```ld
KERNEL_VMA = 0xFFFFFFFF80000000;   /* higher-half virtual base */
KERNEL_LMA = 0x1000000;            /* physical load address (16 MB) */
```

VMA（Virtual Memory Address）是链接器在生成符号地址时使用的基准——所有函数指针、全局变量地址、linker symbol 都会以这个基址为起点计算。LMA（Load Memory Address）是程序实际被加载到内存中的物理位置。这两者的分离正是 higher-half 内核的核心技巧。

链接器通过一行看似简单的赋值设定了当前位置计数器：

```ld
. = KERNEL_VMA + KERNEL_LMA;
```

当前位置变成了 `0xFFFFFFFF80100000`。这个加法非常巧妙：它让所有符号的地址都在 higher-half 范围内，同时通过 `AT()` 指令把实际加载地址指定为物理地址。来看 `.text` 段的定义：

```ld
.text : AT(ADDR(.text) - KERNEL_VMA) ALIGN(4096) {
    *(.text.start)         /* _start MUST be first */
    *(.text .text.*)
    *(.rodata .rodata.*)
}
```

`AT(ADDR(.text) - KERNEL_VMA)` 就是那个"魔法公式"。如果 `.text` 段的 VMA 是 `0xFFFFFFFF80100000`，那么 `ADDR(.text) - KERNEL_VMA = 0x100000`，它会被加载到物理地址 `0x100000`。ELF 文件的程序头中，`p_vaddr` 记录虚拟地址，`p_paddr` 记录物理地址，ELF loader 按 `p_paddr` 加载段数据——这就是整个 higher-half 机制的底层支撑。

段排列方面，`.text` 段排在最前面，里面特别强调了 `*(.text.start)` 必须最先出现——这是 `_start` 所在的小段，放在最前面确保它是 ELF 文件的第一个字节。代码段之后是 `.data`（可写已初始化数据），然后是 `.init_array`（全局构造函数指针），再然后是 `.bss`（未初始化数据）。`.init_array` 段的两个边界 linker symbol 值得特别关注：

```ld
.init_array : AT(ADDR(.init_array) - KERNEL_VMA) ALIGN(8) {
    __init_array_start = .;
    KEEP(*(.init_array .init_array.*))
    __init_array_end   = .;
}
```

`__init_array_start` 和 `__init_array_end` 在 `crt_stub.cpp` 中被用来遍历构造函数数组。`KEEP()` 指令告诉链接器即使启用了 `--gc-sections` 也不要把这些条目优化掉——这很重要，因为构造函数指针是在编译时由编译器放入 `.init_array` 的，没有显式的代码引用它们，链接器的垃圾回收会认为这些指针是"无用的"然后把它们删掉。

`.bss` 段的定义比较朴素：

```ld
.bss : ALIGN(4096) {
    __bss_start = .;
    *(.bss .bss.*)
    *(COMMON)
    __bss_end = .;
}
```

注意 `.bss` 没有 `AT()` 指令——BSS 段在 ELF 文件中不占空间（它只有大小没有内容），所以不需要指定加载地址。`__bss_start` 和 `__bss_end` 这两个 linker symbol 在 `boot.S` 中被用来确定清零的范围。

栈放在 `.bss` 之后，16KB 大小，用 `NOLOAD` 标记表示不占文件空间：

```ld
.stack (NOLOAD) : ALIGN(4096) {
    . = . + 0x4000;          /* 16 KB stack */
    __kernel_stack_top = .;
}
```

`__kernel_stack_top` 是栈顶地址——x86 上栈向下增长，所以 `rsp` 被初始化为这个值。

和 mini kernel 的构建方式对比一下会发现，mini kernel 根本没有正式的链接脚本——它是被编译成 ELF 后再 `objcopy -O binary` 转成 flat binary 的，段布局完全靠默认行为。大内核有了自己的链接脚本，就能精确控制每个段的位置和对齐，这是从"玩具内核"迈向"正规内核"的第一步。

## 第二步——boot.S：六步启动流程的精讲

`kernel/arch/x86_64/boot.S` 是大内核执行的第一段代码。当 mini kernel 通过 `jmp *%0` 跳到大内核的 `_start` 时，CPU 正处于 64 位长模式，中断已被禁用，页表是 identity mapping。我们面前是一片未经初始化的内存，任务是在调用任何 C++ 代码之前把运行环境准备好。整个流程分六步，我们来逐一看。

先看文件头的声明：

```asm
.section .text.start, "ax"
.code64

.global _start
.type   _start, @function
```

`.section .text.start, "ax"` 把这段代码放进 `.text.start` 段——还记得链接脚本里 `*(.text.start)` 被放在最前面吗？这就保证了 `_start` 是整个内核 ELF 文件的第一个字节。`"ax"` 表示 allocatable 和 executable，这是代码段的标准属性。

**Step 1：禁用中断**

```asm
_start:
    cli                           # disable interrupts
```

看起来有点多余——mini kernel 在跳转之前已经 `cli` 了。但这是一种防御性操作。如果大内核将来被其他方式加载（比如 GRUB），我们不能假设中断一定是禁用的。在大内核建立自己的 IDT 之前，任何中断都会触发 double fault 甚至 triple fault，直接重启。

**Step 2：设置栈**

```asm
    movq  $__kernel_stack_top, %rsp  # set stack pointer
    xorq  %rbp, %rbp                # clear base pointer (end of call chain)
```

`$__kernel_stack_top` 是链接脚本定义的栈顶地址，`%rsp` 指向它。`xorq %rbp, %rbp` 把基址指针清零——这不是装饰性的，栈回溯（stack unwinding）代码通过检查 `%rbp == 0` 来判断已经到达栈底，如果不清零，调试器会试图回溯到随机地址。

这里有一个微妙但重要的设计选择需要讨论。`__kernel_stack_top` 的值是 higher-half 虚拟地址（类似 `0xFFFFFFFF8010XXXX`），而当前 CPU 运行在 identity mapping 的物理地址空间中——页表没有映射 higher-half 区域。严格来说，直接用这个地址应该会触发 page fault。但实际能工作的原因是：mini kernel 在加载大内核之前调用了 `identity_map_up_to(highest_phys)`，确保所有大内核用到的物理地址都被 identity mapping 覆盖。而大内核的 `boot.S` 被加载到了物理地址 `0x1000000`，CPU 在物理地址空间执行——这里引用的 linker symbol 的值虽然编码为 higher-half 地址，但在当前 milestone 中，这些地址的物理地址部分（`0x10XXXX`）恰好在 identity mapping 范围内。

⚠️ 不过这种"碰巧能工作"的状态不是最终方案。后续 milestone 中我们需要在 `boot.S` 的早期建立 higher-half 页表映射，把 `0xFFFFFFFF80000000` 映射到 `0x1000000`，然后通过一个相对于 RIP 的 `jmp` 切换到 higher-half 地址空间。到那时，linker symbol 的 higher-half 地址才能被"正式"使用。

**Step 3：清零 BSS**

```asm
    movq  $__bss_start, %rdi       # destination address
    movq  $__bss_end, %rcx         # end address
    subq  %rdi, %rcx               # byte count = end - start
    xorq  %rax, %rax               # fill value = 0
    rep stosb                      # zero-fill BSS, rcx times
```

五条指令完成整个 BSS 清零，非常优雅。`rep stosb` 是 x86 上最紧凑的内存填充指令——它内部是一个 hardware loop，把 `%rax` 的低字节写入 `%rdi` 指向的内存，每次 `%rdi` 自增 1、`%rcx` 自减 1，直到 `%rcx` 为 0。比手写 `movb` 循环效率高得多。

你可能会问：ELF loader 在加载 PT_LOAD 段时，如果 `p_memsz > p_filesz`，不是已经用 `memset` 把多出来的部分清零了吗？为什么还要再做一遍？答案是防御性编程。虽然我们的 ELF loader 确实会处理 BSS，但这个行为是 ELF loader 的实现细节，不是可靠的契约——如果将来换了 bootloader，或者 ELF 文件的段布局变了，BSS 可能就没有被正确清零。五条指令的开销换来的是消除一整类"未初始化变量包含垃圾值"的 bug，这种 bug 在内核开发中是最阴险的——每次启动时垃圾值可能不同，bug 表现也不一样，极难复现。

这里有一个关于 `%rdi` 被破坏的细节值得注意。按 System V AMD64 调用约定，mini kernel 跳转到大内核时，`%rdi` 应该包含 BootInfo 指针。但 `rep stosb` 把 `%rdi` 破坏了（它用 `%rdi` 作为目标地址）。当前 milestone 的解决方案很简单——在调用 `kernel_main` 之前直接 `xorq %rdi, %rdi` 传入 NULL，因为当前 `kernel_main` 不接受参数。但 `boot.S` 的注释里留了一个 TODO：未来需要传递 BootInfo 时，必须在 BSS 清零之前保存 `%rdi`，之后再恢复。

**Step 4：调用全局构造函数**

```asm
    call  _init_global_ctors
```

这一行调用 `crt_stub.cpp` 中定义的 `_init_global_ctors()`，遍历 `.init_array` 段中的每个函数指针并调用。虽然当前大内核没有任何全局对象需要构造（`.init_array` 是空的），但这个机制现在就建立好了，将来添加全局对象时不会踩到"构造函数没被调用"的隐蔽 bug。

**Step 5：进入 C++ 世界**

```asm
    xorq  %rdi, %rdi              # NULL→rdi: BootInfo* (unused for now)
    call  kernel_main
```

把 `%rdi` 清零传入 NULL，然后 `call kernel_main`。到这里，我们从汇编世界正式进入了 C++ 世界。

**Step 6：永不返回的保护**

```asm
.halt:
    cli                           # disable interrupts
    hlt                           # halt processor
    jmp   .halt                   # in case of NMI, re-halt
```

如果 `kernel_main` 意外返回（它标记了 `[[noreturn]]` 所以理论上不应该），CPU 会落到这里。`cli; hlt` 让 CPU 进入低功耗停机状态，`jmp .halt` 处理 NMI（不可屏蔽中断）——即使 NMI 唤醒了 CPU，它也会立刻重新 halt。这种"halt + jump back"的模式是内核开发中的标准做法，Linux 内核的 `panic()` 最终也是类似的逻辑。

## 第三步——crt_stub.cpp：freestanding 环境的生存指南

在 `-ffreestanding -nostdlib` 环境下，编译器期望某些符号的存在——即使是内核，链接时也需要这些桩函数。`crt_stub.cpp` 提供的就是这些最基本的"运行时支撑"。

**纯虚函数处理**

```cpp
[[noreturn]] void __cxa_pure_virtual() {
    __asm__ volatile("cli");
    __asm__ volatile("outb %0, %1" : : "a"((uint8_t)'V'), "Nd"((uint16_t)0xE9));
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}
```

这个函数你永远不希望被调用。它处理的是一种编程错误：通过虚函数表调用了一个纯虚函数。在正常运行中这不应该发生，但如果基类构造函数中调用了虚函数，或者有人通过野指针调用了虚函数，就会走到这里。我们的实现输出字符 'V' 到 debug I/O 端口 `0xE9`（QEMU 的 debug console），然后永久 halt。这是一种非常轻量的调试手段——如果 QEMU 的 debug.log 里出现了 'V'，你就知道有纯虚函数调用发生了。

**栈溢出检测**

```cpp
[[noreturn]] void __stack_chk_fail() {
    __asm__ volatile("cli");
    __asm__ volatile("outb %0, %1" : : "a"((uint8_t)'S'), "Nd"((uint16_t)0xE9));
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}
```

当前编译时用了 `-fno-stack-protector`，所以这个函数理论上永远不会被调用。但如果将来有人启用了栈保护，编译器会在每个函数的栈帧里插入 canary 值，返回前检查——如果被篡改，说明发生了栈缓冲区溢出，直接走到这里。输出 'S' 到 debug console 并 halt。

**atexit 桩函数**

```cpp
int __cxa_atexit(void (*)(void*), void*, void*) {
    return 0;
}
```

内核永远不会"退出"，所以 `atexit` 注册的回调没有意义。直接返回 0 表示注册成功，但我们根本不记录任何东西。

**全局构造函数遍历**

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
```

这是整个文件里最有实质内容的函数。它从链接脚本提供的两个 linker symbol 获取构造函数数组的起止地址，逐个调用。这个机制是 C++ ABI 的一部分：编译器编译一个有非平凡构造函数的全局对象时，会生成一个构造函数，把函数指针放到 `.init_array` 段里。链接时所有条目被合并成连续数组。运行时，C 运行时负责遍历并调用——在普通程序中这是 `__libc_csu_init` 做的，在我们的内核里就是 `_init_global_ctors()` 做的。

循环里的 `nullptr` 检查不是多此一举——链接器可能为了对齐而在 `.init_array` 中插入零填充的条目，调用空指针会导致跳转到地址 0，在内核中这是一个确定的 page fault。

**operator new/delete**

这几个运算符的实现是直接 halt——大内核目前没有堆分配器：

```cpp
void* operator new(unsigned long size) {
    (void)size;
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}
```

它们被声明在 `extern "C"` 块之外，因为需要 C++ 的 name mangling（`operator new` 的 mangled name 是 `_Znwm`）。如果某个构造函数或虚析构函数触发了 `new`，CPU 会直接 halt——这是一种 fail-fast 策略，比返回垃圾指针然后引发随机 crash 要好得多。

## 第四步——kernel_main：从喧嚣归于宁静

经过链接脚本的布局、boot.S 的底层初始化、crt_stub 的运行时支撑，我们终于到达了大内核的 C++ 入口函数。在 milestone 009 中，它做的事情极其简单：

```cpp
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

初始化串口（COM1，115200 8N1 配置），打印里程碑消息，永久 halt。你别小看这几行代码——如果 `[BIG] Big kernel running @ 0x1000000` 这行字出现在 QEMU 串口输出中，它证明了一整个启动链条的正确性：MBR 加载 Stage2，Stage2 加载 mini kernel，mini kernel 初始化硬件、从磁盘读取大内核 ELF、解析并加载段、跳转到 `_start`，`_start` 设栈清 BSS 跑构造函数，最终到达这里。链条上任何一环断裂，这行字都不会出现。

`extern "C"` 声明确保 `kernel_main` 使用 C 语言的 name mangling——这样 `boot.S` 中的 `call kernel_main` 才能正确链接到这个函数。如果去掉 `extern "C"`，编译器会对函数名进行 C++ mangling（变成 `_Z11kernel_mainv` 之类的），链接时就会报 undefined reference。

## 第五步——mini kernel 端的跳转：接力棒的交接

虽然这一章的核心是大内核自身的启动基础设施，但理解 mini kernel 是怎么把控制权交出来的同样重要。在 mini kernel 的 `main.cpp` 中，`load_big_kernel()` 返回大内核的物理入口地址后，mini kernel 通过一个内联汇编完成最终跳转：

```cpp
__asm__ volatile(
    "cli            \n\t"  // disable interrupts before handoff
    "jmp *%0        \n\t"
    :
    : "r"(entry)
    : "memory");
```

`"r"(entry)` 让编译器把入口地址放到任意一个通用寄存器，`jmp *%0` 执行间接跳转。`"memory"` clobber 告诉编译器这个内联汇编可能读写内存（虽然 `jmp` 本身不读写，但跳转后的代码会），防止编译器把内存操作重排到 `jmp` 之后。跳转前最后一次 `cli` 确认中断被禁用——一旦跳转到大内核，就没有 IDT 来处理中断了。

这个跳转有一个隐含前提：`entry` 是物理地址（由 ELF loader 的 higher-half 转换逻辑 `e_entry - KERNEL_VMA` 算出），mini kernel 的代码运行在 identity mapping 下，所以 `jmp` 跳到的是物理地址，大内核的 `_start` 也在物理地址空间执行。这是整个启动链条中设计最简洁的一环——不需要切换页表，不需要切换模式，一个 `jmp` 就完成了控制权的交接。

## 点火测试

现在我们来验证整个链路。构建并运行：

```bash
cd build && cmake -B . -DCMAKE_BUILD_TYPE=Debug -S .. && cmake --build . -j$(nproc) && make run
```

串口输出的关键部分应该是这样的：

```
Cinux Mini Kernel v0.1.0
[INIT] Setting up GDT...
[INIT] GDT loaded successfully.
[INIT] Setting up IDT...
[INIT] IDT loaded successfully.
[INIT] Initializing ATA controller...
[INIT] ATA controller initialized successfully (status=0x50).
...
[LOADER] Big kernel loaded successfully.
[LOADER] Entry point: 0x1000000
[BIG] Big kernel running @ 0x1000000
```

当 `[BIG] Big kernel running @ 0x1000000` 这行字出现的时候，说实话当时真的搞了一晚上才看到——整个启动链条从 MBR 到 Stage2 到 mini kernel 到大内核，走完了从磁盘上的字节到屏幕上的一行文字的完整旅程。从 mini kernel 的 `[LOADER] Entry point: 0x1000000` 到大内核的 `[BIG]` 输出之间，就是 boot.S 那六步在做的事情：禁中断、设栈、清 BSS、跑构造函数、调 kernel_main。如果 BSS 没清对，或者栈设错了，或者构造函数机制有问题——任何一步出状况，这行字都不会出现，取而代之的会是一个沉默的 QEMU 窗口或者 triple fault 重启。

## 一些值得记住的坑

搞大内核启动基础设施的时候，最容易踩的第一个坑是 linker symbol 的地址理解问题。`__kernel_stack_top`、`__bss_start`、`__bss_end` 这些符号的值是 higher-half 虚拟地址（`0xFFFFFFFF8010XXXX`），而不是物理地址。在 identity mapping 环境下，这些地址不能直接使用——你需要的物理地址是 `symbol - KERNEL_VMA`。当前 milestone 能工作是因为 identity mapping 恰好覆盖了大内核所在的物理区域，但这不是一个可靠的长期方案。

第二个坑关于 `rep stosb` 破坏 `%rdi`。`boot.S` 的 BSS 清零操作用 `%rdi` 作为目标地址，执行完之后 `%rdi` 不再是 mini kernel 传入的 BootInfo 指针了。如果将来需要传递 BootInfo 给 `kernel_main`，必须在 BSS 清零之前把 `%rdi` 保存到一个临时位置（比如一个全局变量），清零之后再恢复。mini kernel 的 `boot.S` 采取了更正确的做法——它在清零 BSS 之前就把 `%rdi` 存到了全局变量 `__boot_info_ptr` 里。

第三个坑是 `.init_array` 条目被链接器优化掉的问题。如果链接时启用了 `--gc-sections`，未显式引用的 `.init_array` 条目会被当作"无用代码"删掉。我们的链接脚本用 `KEEP(*(.init_array .init_array.*))` 来防止这个问题。如果你手写了链接脚本但忘了加 `KEEP`，将来添加全局对象时会收获一个非常隐蔽的 bug：构造函数没有被调用，对象处于未初始化状态，但编译器不报任何错。

## 为什么选择 Higher-Half

读到这里的同学可能会好奇：为什么一开始就选择 higher-half 布局？直接 identity mapping 不是更简单吗？

确实，identity mapping 在初期更简单——内核在物理地址 `0x1000000` 上运行，虚拟地址等于物理地址，不用操心页表映射。但问题是，当我们将来要实现用户空间进程时，进程的虚拟地址空间从 `0x0` 开始向上增长。如果内核也在低地址，就必须为每个进程的地址空间"挖掉"内核占用的那部分，这大大增加了地址空间管理的复杂性。

Higher-half 的好处是内核和用户空间天然隔离：用户空间完整拥有 `0x0` 到 `0x7FFFFFFFFFFF` 的低地址区域（128TB），内核独占高地址区域。选择 `0xFFFFFFFF80000000` 而不是更高地址（比如 `0xFFFF800000000000`）作为基址，是因为前者是"负 2GB"偏移——`-mcmodel=kernel` 可以让编译器用 32 位相对地址引用内核内部符号，生成更紧凑的指令编码。Linux 选择了相同的基址（`__START_KERNEL_map`），Windows 内核也用 higher-half 布局——这不是巧合，而是 x86-64 canonical address 设计的直接结果。

## 收尾

到这里，大内核的启动基础设施就完全搭好了。链接脚本定义了 higher-half 内存布局，boot.S 完成了从裸机到 C++ 的六步桥接，crt_stub 提供了 freestanding 环境下的最小运行时支撑，kernel_main 打出了那行标志着整个启动链走通的 `[BIG]` 输出。

从架构角度看，这一步的意义在于 mini kernel 不再是"终点"，而成了"起点"——它只是一个引导加载器，负责把真正的大内核从磁盘搬运到内存并启动。大内核才是未来承载所有操作系统功能的主角。接下来，大内核需要自己的串口驱动、kprintf、内存管理、中断系统——每一项都是在大内核的 higher-half 地址空间中重新构建。但那是后续 milestone 的事情了。

本章对应 milestone：`009_big_kernel_boot`
上一章：[008 - 磁盘加载与 ELF 解析](008-mini-kernel-disk-and-loader.md)
下一章预告：大内核 I/O 与串口驱动（milestone 009B）
