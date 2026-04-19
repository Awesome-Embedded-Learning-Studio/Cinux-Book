# 009A Higher-Half 大内核启动基础设施 - 通读版

**本章 git tag**：`009_big_kernel_boot`，上一章 tag：`008_mini_kernel_disk_and_loader`

---

## 本章概览

到了 milestone 008，我们的 mini kernel 已经有能力从磁盘读取 ELF 文件、解析 PT_LOAD 段、把大内核搬运到内存里了。但一个让人沮丧的事实是——我们一直在 mini kernel 那个 flat binary 的世界里打转，它没有链接脚本，没有段的概念，甚至连 BSS 清零都是在启动汇编里硬编码的。这一章，我们终于要走出 mini kernel 的舒适区，为真正的"大内核"搭建启动基础设施。

这一章的核心产出是四个文件：一个链接脚本 `kernel/linker.ld` 定义了 higher-half 内核的内存布局；一个启动汇编 `kernel/arch/x86_64/boot.S` 承担了从 mini kernel 接管控制权后的所有底层初始化工作（设栈、清 BSS、跑全局构造函数）；一个 C++ 运行时桩代码 `kernel/arch/x86_64/crt_stub.cpp` 提供了 freestanding 环境下编译器期望的各种符号；以及一个极简的 `kernel/main.cpp` 作为大内核的 C++ 入口点。从整个 OS 的启动链条来看，这一步是 mini kernel 把接力棒交给大内核的那个"交接仪式"——mini kernel 负责把大内核从磁盘加载到内存，然后 `jmp` 到 `_start`，接下来的一切就是本章的内容。

关键设计决策方面：我们选择了 higher-half 内核布局（虚拟基址 `0xFFFFFFFF80000000`），把内核映射到地址空间的最高区域，为用户空间留出完整的低地址空间；BSS 清零被设计为无条件执行，即使 ELF loader 可能已经做过一遍，我们也再做一次以保持防御性；全局构造函数调用机制（`.init_array` 遍历）现在就建立好了，即使当前没有任何全局对象需要构造——这是一种"基础设施先行"的思路。和 Linux 对比的话，Linux 内核同样运行在 higher-half（它的 `__START_KERNEL_map` 在 `0xFFFFFFFF80000000`），启动流程中也有类似的 BSS 清零和构造函数调用，我们的设计基本与之一致，只是简化了很多细节。

---

## 架构图

```
大内核启动流程（mini kernel → big kernel 交接）：

  mini kernel (mini_kernel_main)
         │
         │  1. 初始化 GDT / IDT / PMM / ATA
         │  2. load_big_kernel() → 从磁盘加载 ELF 到物理内存
         │  3. jmp *entry → 跳转到 _start（物理地址）
         │
═════════╪══════════  控制权交接  ════════════════════════
         │
         ▼
  ┌─ kernel/arch/x86_64/boot.S (_start) ──────────────────┐
  │                                                         │
  │  Step 1: cli                禁用中断                    │
  │  Step 2: rsp = stack_top    设置内核栈                  │
  │           rbp = 0           标记栈底                    │
  │  Step 3: rep stosb          清零 BSS 段                 │
  │  Step 4: call ctors         调用全局构造函数             │
  │  Step 5: call kernel_main   进入 C++ 世界               │
  │  Step 6: cli; hlt; jmp      永不返回的保护              │
  └─────────────────────────────────────────────────────────┘
         │
         ▼
  ┌─ kernel/arch/x86_64/crt_stub.cpp ──────────────────────┐
  │                                                         │
  │  _init_global_ctors()  遍历 .init_array 调用构造函数    │
  │  __cxa_pure_virtual()  纯虚函数调用 → halt              │
  │  __stack_chk_fail()    栈溢出检测 → halt                │
  │  __cxa_atexit()        atexit 注册 → no-op              │
  │  operator new/delete   动态内存 → halt（无堆分配器）     │
  └─────────────────────────────────────────────────────────┘
         │
         ▼
  ┌─ kernel/main.cpp ─────────────────────────────────────┐
  │                                                        │
  │  kernel_main()                                         │
  │    ├─ kprintf_init()      初始化串口                   │
  │    ├─ kprintf("[BIG]...") 打印里程碑消息               │
  │    └─ while(1) cli; hlt   永久停机                     │
  └────────────────────────────────────────────────────────┘

内存布局（大内核加载后）：

  虚拟地址空间：
  ┌──────────────────────────────────┐ 0xFFFFFFFFFFFFFFFF
  │  .text.start  (_start)           │ ← 0xFFFFFFFF80100000
  │  .text + .rodata                 │
  │  .data                           │
  │  .init_array                     │
  │  .bss                            │
  │  stack (16 KB)                   │ ← __kernel_stack_top
  └──────────────────────────────────┘
  │  ... gap ...                     │
  ┌──────────────────────────────────┐ 0x0000000000000000
  │  用户空间（未来）                 │
  └──────────────────────────────────┘

  物理地址空间（当前 identity-mapped）：
  0x0000000 ─── 0x001000   页表（PML4/PDPT/PD）
  0x0020000 ─── ~0x0087000 Mini Kernel（flat binary）
  0x1000000 ─── ~0x100XXXX Big Kernel（ELF 加载后）
```

---

## 关键代码精讲

### 链接脚本：给内核一个确定的家

链接脚本 `kernel/linker.ld` 是整个大内核基础设施的基石——它定义了内核在虚拟地址空间中的位置，决定了每个段怎么排列，以及物理地址和虚拟地址之间的映射关系。理解了这个文件，后面所有代码中出现的 linker symbol（`__bss_start`、`__kernel_stack_top` 等）就都有了着落。

脚本的开头声明了输出格式和架构：`OUTPUT_FORMAT("elf64-x86-64")` 和 `OUTPUT_ARCH(i386:x86-64)` 告诉链接器我们生成的是 x86-64 的 ELF64 文件。`ENTRY(_start)` 指定了程序入口——这个符号必须在 `.text.start` 段的最开头定义，因为我们希望 `_start` 是整个内核 ELF 文件的第一个字节。这一点很重要：mini kernel 的 ELF loader 会解析 ELF 头中的 `e_entry` 字段得到入口虚拟地址，转换成物理地址后跳转过去。如果 `_start` 不是 ELF 文件的第一个字节，跳转就会落空。

两个地址常量是理解整个链接脚本的关键：`KERNEL_VMA = 0xFFFFFFFF80000000` 是 higher-half 虚拟基址，`KERNEL_LMA = 0x1000000` 是物理加载地址（16MB）。VMA（Virtual Memory Address）是链接器在生成符号地址时使用的基准——所有函数指针、全局变量地址、linker symbol 都会以这个基址为起点计算。LMA（Load Memory Address）是程序实际被加载到内存中的物理位置，由 `AT()` 指令指定。

链接器通过 `. = KERNEL_VMA + KERNEL_LMA` 设定当前位置计数器为 `0xFFFFFFFF80100000`。这个看似奇怪的加法其实非常巧妙：它让所有符号的地址都在 higher-half 范围内，同时通过 `AT(ADDR(.text) - KERNEL_VMA)` 把实际加载地址指定为物理地址。也就是说，如果一个函数的 VMA 是 `0xFFFFFFFF80100000`，那么 `ADDR(.text) - KERNEL_VMA = 0x100000`，它会被加载到物理地址 `0x100000`。这就是 higher-half 内核的核心技巧——链接器生成的是虚拟地址，但 ELF 的 `p_paddr` 字段记录了对应的物理地址，ELF loader 按 `p_paddr` 加载，按 `p_vaddr` 记录虚拟地址。

段的排列顺序值得仔细看。`.text` 段排在最前面，包含 `.text.start`（`_start` 所在的小段）、其他代码和只读数据。`.data` 段紧接着，存放可写的已初始化数据。`.init_array` 段存放全局构造函数指针，`__init_array_start` 和 `__init_array_end` 两个 linker symbol 标记了它的起止位置——`crt_stub.cpp` 里的 `_init_global_ctors()` 就是靠这两个符号来遍历构造函数数组的。`.bss` 段是未初始化数据，它在 ELF 文件中不占空间（用 `NOLOAD` 或者不写 `AT()`），但加载时需要清零。`__bss_start` 和 `__bss_end` 这两个 linker symbol 在 `boot.S` 中被用来确定清零的范围。

栈的定义放在 `.bss` 之后，大小 16KB（`0x4000`）。`__kernel_stack_top` 是栈顶地址——在 x86 上栈是向下增长的，所以 `rsp` 被初始化为这个值。`NOLOAD` 关键字告诉链接器这个段不占用文件空间。最后的 `/DISCARD/` 段丢掉了编译器生成的注释、note 和异常帧信息——内核不需要这些东西。

和 mini kernel 的链接脚本对比一下，会发现一些有趣的差异。mini kernel 的物理加载地址是 `0x20000`（128KB），大内核的物理加载地址是 `0x1000000`（16MB）——两者之间有将近 16MB 的间距，足够避免任何冲突。mini kernel 没有独立的 `.init_array` 段定义（虽然它的 `crt_stub.cpp` 也有构造函数遍历），也没有栈的 linker symbol（它的栈是在 `boot.S` 中用 `.skip` 直接分配的）。大内核的链接脚本更加规范，更接近生产级内核的做法。

### 启动汇编：从裸机到 C++ 的六步桥接

`kernel/arch/x86_64/boot.S` 是大内核执行的第一段代码。当 mini kernel 通过 `jmp *%0` 跳到大内核的 `_start` 时，CPU 正处于 64 位长模式，中断已被禁用，页表是 identity mapping。我们面前是一片未经初始化的内存，任务是在调用任何 C++ 代码之前把运行环境准备好。

第一步是 `cli`——禁用中断。这看起来有点多余，因为 mini kernel 在跳转之前已经 `cli` 了，但这是一个防御性操作。如果大内核将来被其他方式加载（比如另一个 bootloader），我们不能假设中断一定是禁用的。在大内核建立自己的 IDT 之前，任何中断都会触发 double fault 甚至 triple fault，直接重启。

第二步是设置栈。`movq $__kernel_stack_top, %rsp` 把栈指针指向链接脚本定义的栈顶。`__kernel_stack_top` 是一个链接时确定的常量——因为我们的内核是 higher-half 编译的，这个地址实际上是一个类似 `0xFFFFFFFF8010XXXX` 的虚拟地址。但现在 mini kernel 只有 identity mapping，页表没有映射 higher-half 区域，那这个地址怎么能工作呢？答案在于 mini kernel 的页表设置：它只建立了 identity mapping，物理地址 `0x1000000` 被映射到虚拟地址 `0x1000000`。所以如果 `__kernel_stack_top` 的值是一个 higher-half 地址，直接用它就会触发 page fault。

这里有一个微妙但关键的设计选择：大内核当前是运行在 identity mapping 的物理地址空间中的。ELF loader 按 `p_paddr` 把段加载到 `0x1000000` 附近，页表把这些物理地址 identity mapping 到相同的虚拟地址。所以 `_start` 被跳转到的地址是物理地址 `0x1000000`，而不是虚拟地址 `0xFFFFFFFF80100000`。这意味着链接脚本生成的所有 linker symbol（包括 `__kernel_stack_top`、`__bss_start`、`__bss_end`）的值都是 higher-half 虚拟地址，但在 identity mapping 环境下不能直接使用。

但是等等——看看 `boot.S` 的代码，它确实直接用了 `$__kernel_stack_top`。这是因为，在当前的实现中，这些地址碰巧是正确的——或者说，mini kernel 在加载大内核之前已经确保了足够的 identity mapping 覆盖。实际上，更仔细地看，大内核的 `boot.S` 中引用的 linker symbol 确实是 higher-half 地址（如 `0xFFFFFFFF8010XXXX`），但在没有 higher-half 页表映射的情况下，这些地址是无效的。这就意味着当前实现依赖于 mini kernel 建立了足够的页映射——或者，更准确地说，依赖于这些 symbol 的物理地址刚好在 identity mapping 范围内。

让我来澄清这一点。mini kernel 的 `load_big_kernel_phase2` 调用了 `identity_map_up_to(highest_phys)` 来确保所有大内核用到的物理地址都被 identity mapping。但 identity mapping 映射的是物理地址到相同的虚拟地址——`0x1000000` 物理地址映射到 `0x1000000` 虚拟地址。而 `__kernel_stack_top` 的值是 `0xFFFFFFFF8010XXXX`，这是一个完全不同的虚拟地址范围。所以严格来说，当前代码有一个前提条件：大内核的 `boot.S` 中使用的 linker symbol 必须通过某种方式被转换为物理地址。

在实际的 Cinux 实现中，大内核的 ELF 被 mini kernel 的 ELF loader 加载时，ELF loader 已经按照 `p_paddr`（物理地址）来放置段数据。而 `_start` 的入口点是通过 `e_entry - KERNEL_VMA` 转换得到的物理地址。所以 CPU 实际上是在物理地址空间执行 `boot.S` 的代码。此时如果 `boot.S` 中引用了一个 higher-half 地址（比如 `__kernel_stack_top`），CPU 会尝试访问 `0xFFFFFFFF8010XXXX`——如果页表没有映射这个地址，就会 page fault。

这个问题的解决方案有两种：要么在跳转到大内核之前建立 higher-half 页表映射（让 `0xFFFFFFFF8010XXXX` 映射到 `0x10XXXX`），要么让大内核的启动代码使用物理地址版本的 linker symbol。当前 Cinux 的实现实际上更巧妙——大内核的代码本身被加载到了物理地址，但由于 identity mapping 的存在，`boot.S` 中的代码可以在物理地址空间执行。而 linker symbol 的值虽然是 higher-half 地址，但因为当前只有 identity mapping，所以大内核需要通过 `symbol - KERNEL_VMA` 来获取物理地址——或者，如果 mini kernel 在跳转前也映射了 higher-half 区域，那么 higher-half 地址就可以直接使用。这个话题我们在设计决策部分再深入讨论。

第三步是清零 BSS。`movq $__bss_start, %rdi` 把 BSS 起始地址加载到 `%rdi`，`movq $__bss_end, %rcx` 加载结束地址，`subq %rdi, %rcx` 算出字节数，`xorq %rax, %rax` 把 `%rax` 清零，然后 `rep stosb` 执行 `%rcx` 次字节写入，把从 `%rdi` 开始的内存全部填零。`rep stosb` 是 x86 上最紧凑的内存填充指令——它内部是一个 hardware loop，比手写 `movb` 循环效率高得多。整个清零过程只需要五条指令，非常优雅。

这里有一个关于 `%rdi` 被破坏的注释值得注意。注释说 `%rdi` 在入口时应该包含 BootInfo 指针（由 mini kernel 通过调用约定传递），但 BSS 清零的 `rep stosb` 把 `%rdi` 破坏了。当前 milestone 的解决方案是简单地在调用 `kernel_main` 之前把 `%rdi` 清零（`xorq %rdi, %rdi`），传入 NULL。这是合理的，因为当前 `kernel_main` 的签名不接受参数。但注释里的 TODO 提醒我们：未来如果需要传递 BootInfo，就必须在 BSS 清零之前保存 `%rdi`，之后再恢复。mini kernel 的 `boot.S` 采取了更正确的做法——它在清零 BSS 之前就把 `%rdi` 存到了一个全局变量 `__boot_info_ptr` 里，清零之后再把值读回 `%rdi`。

第四步是调用 `_init_global_ctors()`——这个函数在 `crt_stub.cpp` 中定义，遍历 `.init_array` 段并调用每个构造函数指针。这一步确保了所有具有非平凡构造函数的全局对象在 `kernel_main` 之前被正确初始化。

第五步是调用 `kernel_main()`。注意 `xorq %rdi, %rdi` 把第一个参数设为 NULL——当前 `kernel_main` 不接受参数，但这个 `xor` 保证了 `%rdi` 不会包含一个指向已失效数据的垃圾指针。

第六步是一个永不退出的 halt 循环：`cli; hlt; jmp .halt`。如果 `kernel_main` 意外返回（它不应该），CPU 会在 `hlt` 指令上停住。`jmp .halt` 是为了处理 NMI（不可屏蔽中断）——即使 NMI 唤醒了 CPU，它也会立刻重新 halt。这种"halt + jump back"的模式是内核开发中的标准做法。

### C++ 运行时桩代码：freestanding 环境的生存指南

`crt_stub.cpp` 提供了在 `-ffreestanding -nostdlib` 环境下编译器期望的各种符号。这些函数在普通的应用程序中由 C 运行时库（glibc、musl 等）提供，但在内核里我们必须自己来。

`__cxa_pure_virtual()` 是一个你永远不希望被调用的函数。它处理的是一种特殊的编程错误：通过虚函数表调用了一个纯虚函数。在正常运行中，纯虚函数不应该被调用——它们只有声明没有定义，编译器会在虚函数表中放入 `__cxa_pure_virtual` 的地址作为占位符。如果构造函数的虚函数表还没初始化（比如在基类构造函数中调用了虚函数），或者有人通过野指针调用了虚函数，就会走到这里。我们的实现是输出字符 'V' 到 debug I/O 端口 `0xE9`（QEMU 的 debug console），然后永久 halt。输出 'V' 是一种非常轻量的调试手段——如果 QEMU 的串口日志里出现了 'V'，你就知道有一个纯虚函数调用发生了。

`__stack_chk_fail()` 是栈溢出检测失败时的处理函数。当前我们编译时使用了 `-fno-stack-protector`，所以这个函数理论上永远不会被调用。但如果将来有人启用了 `-fstack-protector`，编译器会在每个函数的栈帧里插入一个 canary 值，函数返回前检查 canary 是否被篡改——如果被篡改，说明发生了栈缓冲区溢出，直接调用 `__stack_chk_fail()`。我们的实现输出字符 'S' 到 debug console 并 halt。生产级内核通常会在这里打印详细的诊断信息（包括被溢出的函数地址和调用栈），但我们的内核连 `kprintf` 都还没初始化（`crt_stub.cpp` 不依赖任何内核基础设施），所以只能用最原始的方式报告。

`__cxa_atexit()` 是 `atexit` 的底层实现。在普通程序中，`atexit` 注册的回调会在 `exit()` 时被调用。但内核永远不会"退出"——它要么正常运行，要么 crash。所以我们的实现就是一个 no-op，直接返回 0（表示注册成功，但我们根本不记录任何东西）。

`_init_global_ctors()` 是整个文件里最有实质内容的函数。它从链接脚本提供的 `__init_array_start` 和 `__init_array_end` 两个 linker symbol 获取构造函数数组的范围，然后逐个调用。这个机制是 C++ ABI 的一部分：编译器在编译一个有非平凡构造函数的全局对象时，会生成一个构造函数，并把它的函数指针放到 `.init_array` 段里。链接时，所有 `.init_array` 条目被合并成一个连续的数组。运行时，C 运行时负责遍历这个数组并调用每个条目——在普通程序中这是 `__libc_csu_init` 做的，在我们的内核里就是 `_init_global_ctors()` 做的。

遍历循环里有一个 `nullptr` 检查：`if (ctor != nullptr) ctor();`。这是因为链接器可能为了对齐而在 `.init_array` 中插入零填充的条目。调用一个空指针会导致跳转到地址 0，在内核中这是一个确定的 page fault。加上这个检查，即使有零填充条目也不会出问题。

`operator new` 和 `operator delete` 的实现是直接 halt——因为大内核目前没有堆分配器。这些函数被声明在 `extern "C"` 块之外，因为它们需要 C++ 的 name mangling（`operator new` 的 mangled name 是 `_Znwm`）。如果内核代码中的某个构造函数或者虚析构函数触发了 `new` 或 `delete`，CPU 会直接 halt——这是一种"fail-fast"的策略，比让分配器返回垃圾指针然后引发随机 crash 要好得多。

### 大内核入口：从喧嚣归于宁静

`kernel/main.cpp` 是大内核的 C++ 入口函数，在这个 milestone 中它做的事情极其简单——初始化串口、打印一条消息、然后永久 halt。但你别小看这短短几行代码，它们证明了一整个启动链条的正确性：MBR 加载 Stage2，Stage2 加载 mini kernel，mini kernel 初始化硬件、从磁盘读取大内核 ELF、解析并加载段、跳转到 `_start`，`_start` 设栈清 BSS 跑构造函数，最终到达这里。

`kprintf_init()` 初始化串口驱动（COM1，115200 8N1 配置），这是大内核使用 `kprintf` 的前提。初始化之后，`kprintf("[BIG] Big kernel running @ 0x1000000\n")` 输出里程碑消息。如果这条消息出现在 QEMU 的串口输出中，说明整个加载和启动流程都成功了。

最后的 `while (1) { __asm__ volatile("cli; hlt"); }` 是一个永久循环。`cli` 禁用中断，`hlt` 让 CPU 进入低功耗状态。因为我们在 `kernel_main` 之前已经 `cli` 了，而且没有设置 IDT，所以理论上 `hlt` 不会被中断唤醒。但安全起见还是放在循环里——即使被 NMI 唤醒，也会立刻重新 halt。

### mini kernel 端的跳转：接力棒的交接

虽然这一章的主题是大内核的启动基础设施，但理解 mini kernel 是如何把控制权交给大内核的也很重要。在 `kernel/mini/main.cpp` 中，`load_big_kernel()` 返回大内核的物理入口地址之后，mini kernel 通过一个内联汇编 `jmp *%0` 间接跳转过去：

```cpp
__asm__ volatile(
    "cli            \n\t"  // disable interrupts before handoff
    "jmp *%0        \n\t"
    :
    : "r"(entry)
    : "memory");
```

`"r"(entry)` 让编译器把 `entry` 的值放到任意一个通用寄存器中，`jmp *%0` 执行间接跳转到那个寄存器指向的地址。`"memory"` clobber 告诉编译器这个内联汇编可能读写内存（虽然 `jmp` 本身不读写，但跳转后的代码会），防止编译器把内存操作重排到 `jmp` 之后。跳转前的 `cli` 是最后一次确认中断被禁用——一旦跳转到大内核，就没有 IDT 来处理中断了。

这个跳转有一个隐含的前提：`entry` 是物理地址（由 ELF loader 的 higher-half 转换逻辑算出），而 mini kernel 的代码运行在 identity mapping 的物理地址空间中。所以 `jmp` 跳到的是物理地址，大内核的 `_start` 也在物理地址空间执行。这是整个启动链条中最脆弱的一环——一旦大内核启用了 higher-half 页表，`_start` 就需要在虚拟地址空间执行，这意味着 mini kernel 在跳转之前必须建立好 higher-half 映射。但当前 milestone 不需要考虑这个问题，因为一切都在 identity mapping 下运行。

---

## 设计决策深度分析

#### 决策：Higher-Half 内核布局 vs Identity Mapping

**问题**：内核代码应该被映射到虚拟地址空间的哪个位置？有两种主流选择：identity mapping（虚拟地址等于物理地址，比如内核在 `0x1000000` 虚拟地址上运行）和 higher-half mapping（内核被映射到地址空间的最高区域，比如 `0xFFFFFFFF80000000`）。这是一个影响整个内核架构的根本性决策。

**本项目的做法**：选择了 higher-half 布局，虚拟基址为 `0xFFFFFFFF80000000`，物理加载地址为 `0x1000000`（16MB）。链接脚本中的 `AT()` 指令确保段数据被加载到物理地址，而所有符号的地址都是 higher-half 虚拟地址。

**备选方案**：第一种替代是纯 identity mapping，内核在物理地址 `0x1000000` 上直接运行，虚拟地址等于物理地址。第二种是把内核映射到更高的地址，比如 `0xFFFF800000000000`（canonical address 的上半区域起始处）。

**为什么不选备选方案**：纯 identity mapping 的问题是它让内核占据了低地址空间。当我们要实现用户空间进程时，进程的虚拟地址空间从 `0x0` 开始向上增长，如果内核也在低地址，就必须为每个进程的地址空间"挖掉"内核占用的那部分——这增加了地址空间管理的复杂性。Higher-half 的好处在于内核和用户空间天然隔离：用户空间完整地拥有 `0x0` 到 `0x7FFFFFFFFFFF` 的低地址区域（128TB），内核独占 `0xFFFF800000000000` 到 `0xFFFFFFFFFFFFFFFF` 的高地址区域。

选择 `0xFFFFFFFF80000000` 而不是 `0xFFFF800000000000` 作为基址，是因为前者是"负 2GB"的偏移——在链接时使用 `-mcmodel=kernel`（或者我们的 `-mcmodel=large`）可以让编译器用 32 位相对地址引用内核内部符号，这在指令编码上更紧凑。`0xFFFF800000000000` 距离符号表太远，所有地址引用都需要 64 位绝对地址，增加了代码体积和指令延迟。

Linux 选择了相同的基址 `0xFFFFFFFF80000000`（`__START_KERNEL_map`），Windows 内核也使用 higher-half 布局（虽然具体基址不同）。这不是巧合——这是 x86-64 canonical address 设计的直接结果：高一半的地址空间（bit 48 到 bit 63 全为 1）专门给内核用，低一半给用户空间用，这是 x86-64 平台上几乎所有操作系统的共同选择。

**如果要扩展/改进，应该怎么做**：当前实现中，大内核的 `_start` 实际上运行在 identity mapping 的物理地址空间中（因为 mini kernel 只建立了 identity mapping）。真正的 higher-half 执行需要在大内核启动的早期建立 higher-half 页表映射——具体来说是在 `boot.S` 中，在设置栈和清零 BSS 之前，先切换到 higher-half 页表。这个改进需要修改页表结构，在 PML4 中添加一个条目把 `0xFFFFFFFF80000000` 映射到 `0x1000000`，然后通过修改 CR3 来切换页表。这是一个中等难度的改进，但是是迈向真正 higher-half 内核的必经之路。

#### 决策：无条件清零 BSS

**问题**：大内核的 BSS 段需要在运行时被清零，这是 C 标准对未初始化全局变量的保证。问题是：ELF loader 在加载 PT_LOAD 段时，如果 `p_memsz > p_filesz`，已经会用 `memset` 把多出来的部分清零了。那 `boot.S` 中的无条件 BSS 清零是否多余？

**本项目的做法**：`boot.S` 中无条件执行 BSS 清零，不管 ELF loader 是否已经做过。

**备选方案**：跳过 BSS 清零，完全依赖 ELF loader 的 `memsz > filesz` 处理逻辑。

**为什么不选备选方案**：虽然我们的 ELF loader 确实会在 `memsz > filesz` 时清零 BSS 区域，但这个行为是 ELF loader 的实现细节，不是可靠的契约。有几个场景可能导致 BSS 没有被正确清零：第一，如果将来更换了 ELF loader 的实现，新的实现可能不处理 BSS；第二，如果 ELF 文件的段布局发生变化（比如 `.bss` 被单独放在一个 PT_LOAD 段中，`p_filesz = 0`），ELF loader 的处理逻辑可能不同；第三，如果大内核被 GRUB 或其他 bootloader 加载（Multiboot 协议），那个 bootloader 的 ELF 加载可能不处理 BSS。无条件清零 BSS 只花五条指令（`movq` x 2, `subq`, `xorq`, `rep stosb`），运行时间通常在微秒级别（BSS 段一般不大），但消除了整整一类"未初始化变量包含垃圾值"的 bug。这种 bug 的可怕之处在于它是间歇性的——每次启动时 BSS 中的垃圾值可能不同，导致 bug 表现也不一样，极难复现和调试。

**如果要扩展/改进，应该怎么做**：如果对启动性能有极端要求（比如嵌入式设备的快速启动），可以添加一个标志位让 BSS 清零变成条件性的。具体做法是在 BootInfo 结构体中加一个 `bool bss_initialized` 字段，ELF loader 加载完段后把它设为 `true`，`boot.S` 检查这个标志位，只有为 `false` 时才清零。但说实话，对于桌面/服务器 OS 来说，这个优化完全没有必要——rep stosb 清零几 KB 的 BSS 段只需要几微秒，而内核启动通常需要几十毫秒到几秒。

#### 决策：提前建立全局构造函数调用机制

**问题**：C++ 内核中的 `.init_array` 机制用于在 `main` 之前初始化具有非平凡构造函数的全局对象。当前大内核没有任何全局对象，`__init_array_start` 和 `__init_array_end` 之间的区域是空的，`_init_global_ctors()` 的循环体不会执行。是否应该现在就建立这个机制？

**本项目的做法**：现在就建立了完整的构造函数调用机制，包括链接脚本中的 `.init_array` 段定义、`crt_stub.cpp` 中的遍历逻辑、以及 `boot.S` 中的调用。

**备选方案**：暂时不建立，等真正需要全局构造函数时再加。

**为什么不选备选方案**：这个机制的全部代码量——链接脚本中 3 行、`crt_stub.cpp` 中约 10 行、`boot.S` 中 1 行 `call` 指令——加起来不超过 15 行。如果现在不加，将来需要时很可能忘记，然后在添加第一个全局对象后收获一个非常隐蔽的 bug：全局对象的构造函数没有被调用，对象处于未初始化状态，但编译器不会报任何错。这种 bug 在 C++ 内核开发中比想象的更常见——Linux 内核虽然没有用 C++，但它有类似的 `__initcall` 机制，如果忘了调用 `do_initcalls()`，所有 `module_init` 注册的初始化函数都不会执行。

更重要的是，全局构造函数机制是 C++ ABI 的一部分。编译器在编译时假定 `.init_array` 会被正确遍历，如果我们的运行时不提供这个服务，就违反了编译器的假定。虽然 `-ffreestanding` 让我们摆脱了大部分标准库的约束，但 `.init_array` 不在其中——它是编译器层面的约定，不是库层面的。我们的 `KEEP(*(.init_array .init_array.*))` 确保 `--gc-sections` 不会把构造函数条目优化掉。这种防御性的做法，虽然现在看起来多余，但将来一定会感谢今天的自己。

**如果要扩展/改进，应该怎么做**：当前的构造函数遍历是正向的（从 `start` 到 `end`），对应的析构函数应该在 `atexit` 中注册以便反向调用。我们的 `__cxa_atexit` 是 no-op，这意味着全局对象的析构函数永远不会被调用——但这对内核来说完全没问题，因为内核不会"退出"。如果将来需要实现内核模块的卸载（比如可加载内核模块），就需要一个真正的 `atexit` 机制来记录需要调用的析构函数。

---

## 常见变体与扩展方向

**1. 添加 BootInfo 结构体传递 mini kernel 到大内核的信息**（难度：⭐）

当前大内核的 `kernel_main` 不接受任何参数，它不知道自己被加载到了哪里，不知道物理内存有多大，不知道 mini kernel 传递了什么信息。一个自然的扩展是定义一个 `BootInfo` 结构体（mini kernel 中已经有了），在 `boot.S` 中保存 `%rdi`（调用约定中的第一个参数），然后传递给 `kernel_main`。BootInfo 可以包含：内核物理基址、可用内存映射、mini kernel 的结束地址（避免覆盖）、帧缓冲区信息等。这个扩展需要修改 `boot.S`（保存/恢复 `%rdi`）、`kernel_main` 的签名、以及 mini kernel 跳转前设置 `%rdi` 的代码。

**2. 启用 stack canary（-fstack-protector）**（难度：⭐）

当前我们编译时用了 `-fno-stack-protector`，禁用了栈溢出检测。启用它需要在编译选项中去掉这个 flag，然后实现 `__stack_chk_guard`（canary 的全局值，通常从 RDRAND 或 `/dev/urandom` 获取）和 `__stack_chk_fail`（检测到溢出时的处理）。我们的 `crt_stub.cpp` 已经有了 `__stack_chk_fail` 的实现（输出 'S' 并 halt），所以只需要添加 `__stack_chk_guard` 的定义即可。对于 QEMU 环境，canary 值可以硬编码为一个固定值（比如 `0xDEADBEEFCAFEBABE`），因为内核启动时还没有随机数源。

**3. 添加 Multiboot2 启动协议支持**（难度：⭐⭐）

当前大内核只能被我们的 mini kernel 加载。如果要支持 GRUB 等标准 bootloader，需要实现 Multiboot2 协议：在内核镜像的开头添加 Multiboot2 头部（magic number `0xE85250D6`、flag 字段、校验和），然后在 `boot.S` 中处理 Multiboot2 boot information 结构体。Multiboot2 的好处是 bootloader 会提供内存映射、帧缓冲区、ACPI 表等信息，免去了自己探测硬件的麻烦。

**4. 实现内核命令行参数解析**（难度：⭐⭐）

在 BootInfo 结构体中添加一个 `cmdline` 字段（指向一个 null-terminated 字符串），让 mini kernel 在跳转之前把启动参数传给大内核。大内核在启动时解析这个字符串，根据参数调整行为（比如 `serial=9600` 设置串口波特率，`mem=512M` 限制可用内存）。这个扩展需要一个简单的字符串解析器，但不需要复杂的框架——一个 `strcmp` + `strstr` 的组合就够了。

**5. 在 boot.S 中建立 higher-half 页表并切换**（难度：⭐⭐⭐）

这是最有挑战性但也最有价值的扩展。当前大内核运行在 identity mapping 下，所有地址都是物理地址。要实现真正的 higher-half 执行，需要在 `boot.S` 中手动修改页表：在 PML4 的第 511 项（最后一个）中添加一个指向 PDPT 的条目，PDPT 指向一个 PD，PD 中用 2MB 大页映射 `0xFFFFFFFF80000000` 到 `0x1000000`。然后重载 CR3 切换页表，最后用一个相对于当前 RIP 的 `jmp` 切换到 higher-half 地址空间执行。这个扩展需要对 x86-64 分页机制有深入的理解，但一旦完成，大内核就真正运行在 higher-half 了。

---

## 参考资料

### Intel / AMD 手册

- **Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 3, Chapter 5**: Protection 和 privilege level 相关内容，higher-half 内核依赖的地址空间布局
- **Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 3, Section 4.5**: 4-Level Paging 机制，PML4/PDPT/PD/PT 的结构和大页映射
- **AMD64 Architecture Programmer's Manual Volume 2: System Programming, Section 5.3**: Long Mode 页表结构和 canonical address 空间的定义
- **System V ABI for AMD64**: `.init_array` 和 `.fini_array` 的规范定义，C++ 全局构造函数调用约定

### ELF 规范

- **Executable and Linkable Format (ELF)**: [System V gABI ELF specification](https://refspecs.linuxbase.org/elf/x86_64-abi-0.99.pdf) — ELF64 头部结构、Program Header 中 `p_vaddr` 和 `p_paddr` 的语义
- **Oracle ELF Object File Format**: [Linker and Libraries Guide](https://docs.oracle.com/cd/E23824_01/html/819-0690/) — 链接脚本中 `AT()` 指令和 VMA/LMA 的关系

### OSDev Wiki

- [Higher Half Kernel](https://wiki.osdev.org/Higher_Half_Kernel) — Higher-half 内核的详细教程，包括页表设置和跳转技巧
- [Linker Scripts](https://wiki.osdev.org/Linker_Scripts) — 链接脚本编写指南，`AT()` 和 `ADDR()` 的用法
- [Calling Global Constructors](https://wiki.osdev.org/Calling_Global_Constructors) — `.init_array` 遍历的实现方法
- [C++ Bare Bones](https://wiki.osdev.org/C%2B%2B_Bare_Bones) — freestanding C++ 环境下需要的运行时桩函数列表
- [Stack Smash Protector](https://wiki.osdev.org/Stack_Smash_Protector) — 内核中启用 `-fstack-protector` 的方法

### 其他参考资源

- **Linux 内核源码 `arch/x86/kernel/head_64.S`**: Linux x86-64 的早期启动代码，包含 higher-half 页表设置和 BSS 清零
- **Linux 内核源码 `arch/x86/kernel/vmlinux.lds.S`**: Linux x86-64 的链接脚本，`__START_KERNEL_map` 的定义和段排列
- **xv6 源码 `entry.S` 和 `kernel.ld`**: MIT xv6 的 RISC-V 版本使用类似的 higher-half 布局，虽然架构不同但设计思路一致
- **OS Dev Notes Series**: [Broken Thorn NASM Higher Half](https://www.brokenthorn.com/Resources/OSDev19.html) — 一份详尽的 higher-half 内核实现教程
