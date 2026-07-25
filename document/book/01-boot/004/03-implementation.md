---
title: 03 · 代码路线:E820、ELF、BootInfo、高半映射、内核入口、C++ 运行时
---

# 代码路线:E820、ELF、BootInfo、高半映射、内核入口、C++ 运行时

### 1. 实模式收尾:查 E820 内存图、把内核 ELF 读进内存

趁还在实模式、BIOS 还能用,Stage2 在配完 VESA 之后多调两个函数(都在 [boot.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/common/boot.S)):

```asm
call query_memory_map        # E820 → 物理内存图存到 0x5000
call load_kernel_from_disk   # 把内核 ELF 从 LBA 16 读到物理 0x20000
```

`query_memory_map` 用 BIOS 的 `INT 0x15 AX=0xE820` 问 BIOS"物理内存有哪些区域可用、哪些保留",结果是一串 24 字节的条目(`base/length/type/acpi`),存到 `0x5000`。这张图是后面内核做物理内存管理(PMM)的原料——但我们这一章只负责**收集**,怎么用是后面的事。

`load_kernel_from_disk` 用 001 那套 `INT 0x13 AH=0x42` 扩展读,从 LBA 16 起读 832 个扇区(416KB),倒进物理 `0x20000`。**为什么是 0x20000?** 因为内核的链接脚本([linker.ld](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/linker.ld))把物理落点(LMA)定在了 `0x20000`,读盘地址必须和它对上,否则跳进去就是一堆错位的字节。

> 这里有个源码注释的噪声要提醒:`stage2.S` 里 `load_kernel_from_disk` 那行注释同时写了 "→0x20000" 和 "to 0x88000",看着矛盾,其实说的是两件事:`0x20000` 是载入**起点**、`0x88000` 是载入区**上界**——内核最大占 `0x88000 − 0x20000 = 0x68000 = 416KB`,正好顶到 `0x90000` 的栈之前(见 [build_image.sh](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/scripts/build_image.sh))。所以载入起点是 `0x20000`,以 `linker.ld` 的 `AT(0x20000)`、bootloader 的 `movq $0x20000`、以及 `boot.S` 里 `.set MINI_KERNEL_LOAD_PHYS, 0x20000` 这几处**代码值**为准。顺带一提,`boot_info.h` 和 `boot.S` 的注释里还残留着旧的 `0x10000`,那才是过时噪声,别被它带偏——以代码为准,别以注释为准。

### 2. BootInfo:bootloader 和内核的"交接单"

跳进内核之前,bootloader 得把自己辛苦收集的信息(帧缓冲在哪、内存图长啥样、内核入口是哪)交给内核。Cinux 的做法是定义一个两边共用的结构 [boot_info.h](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/boot_info.h):

```c
typedef struct {
    uint64_t entry_point;      // 内核入口虚拟地址
    uint64_t kernel_phys_base; // 物理载入地址 0x20000
    uint64_t kernel_size;
    uint64_t fb_addr;          // 帧缓冲物理地址
    uint32_t fb_width, fb_height, fb_pitch, fb_bpp;
    uint32_t mmap_count;
    uint32_t _pad;
    MemoryMapEntry mmap[32];   // E820 条目
} __attribute__((packed)) BootInfo;   // 824 字节
```

这里有两个关键设计。一是字段全用显式定长类型外加 `packed`:这个头文件被 bootloader(32 位编译)和内核(64 位编译)同时包含,要是用 `int`、`long` 这种长度随编译模式变的类型,两边对同一字段的理解就会错位,内核读出来全是乱码,所以一律用 `uint32_t`/`uint64_t`,再用 `static_assert(sizeof(BootInfo) == 824)` 把布局钉死。二是交接地址固定在 `0x7000`:bootloader 把 `BootInfo` 填到物理 `0x7000`,内核跳进去后直接去那儿读——这个地址是两边约定好的"信箱"。

`long_mode_entry` 里,bootloader 一边把帧缓冲信息从 `0x6400`、内存图从 `0x5000` 抄进 `0x7000` 的 `BootInfo`,一边把这些字段填实:

```asm
movq $0x7000, %rdi                 # rdi 指向 BootInfo
movq $0xFFFFFFFF80020000, %rax
movq %rax, (%rdi)                  # entry_point
movq $0x20000, %rax
movq %rax, 8(%rdi)                 # kernel_phys_base
# ... 抄帧缓冲、抄内存图 ...
movq $0x7000, %rdi                 # ★ 第一参数 = BootInfo*
movb $0x4A, %al; outb %al, $0xE9   # 'J'
jmp *0xFFFFFFFF80020000            # 跳进内核
```

最后那两行是交接的核心:`rdi = 0x7000`,然后跳转。为什么是 `rdi`?因为 **System V AMD64 ABI 规定函数第一个整型参数走 `%rdi`**。我们把 `BootInfo*` 放进 `rdi` 再跳,内核入口(也按这套 ABI)就能直接拿到它,跟普通函数传参一模一样。

### 3. 高半内核:为什么链接在 0xFFFFFFFF80020000

看内核链接脚本 [linker.ld](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/linker.ld):

```ld
KERNEL_PHYS_BASE = 0x20000;
KERNEL_Virt_BASE = 0xFFFFFFFF80000000;
SECTIONS {
    . = KERNEL_Virt_BASE + KERNEL_PHYS_BASE;   # VMA = 0xFFFFFFFF80020000
    .text : AT(KERNEL_PHYS_BASE) { ... }       # LMA = 0x20000(物理)
    ...
}
```

内核的**虚拟地址(VMA)是 0xFFFFFFFF80020000**(在地址空间的高半),但**物理落点(LMA)是 0x20000**。`. = VMA` 让所有符号按高半地址链接,`AT(LMA)` 告诉 objcopy/bootloader"这些段实际要放在物理 0x20000"。

为什么要把内核放高半?这是 x86_64 内核的惯例:用户态进程占低半地址(0 以下),内核占高半(0xFFFFFFFF80000000 以上),互不干扰,也为以后做用户态/内核态地址隔离铺路。

可问题是:003 我们搭的临时页表只做了**低地址恒等映射**(0~8MB),内核在高半根本没有映射。直接 `jmp 0xFFFFFFFF80020000`,CPU 翻译这个虚拟地址时查不到页表项,当场缺页三重故障。所以 004 在 [long_mode.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/common/long_mode.S) 里**额外搭一条高半映射**:

```asm
# PML4[511] → PDPT(复用同一张 PDPT)
movl $0x2000, %eax; orl $0x03, %eax
movl %eax, 0x1000 + (511 * 8)
# PDPT[510] → PD(复用同一张 PD)
movl $0x3000, %eax; orl $0x03, %eax
movl %eax, 0x2000 + (510 * 8)
```

它的妙处在于复用同一张 PD:低地址(恒等)和高半(0xFFFFFFFF80020000)最终都指向那张记录了物理 0x20000 附近 2MB 页的 PD。于是同一块物理内存,在低地址和高半两个虚拟地址都能访问到——bootloader 用低地址填 BootInfo、读内核;跳过去之后内核用高半地址运行。两边是同一块物理页,只是两扇不同的门。

### 4. 内核入口 boot.S:清 BSS、跑全局构造、调 main

跳进 `0xFFFFFFFF80020000`,落到内核的 [boot.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/arch/x86_64/boot.S) 的 `_start`:

```asm
_start:
    cli
    outb '1', $0xE9                      # ① 到了
    movq $__mini_stack_top, %rsp         # ② 设 8KB 栈
    outb '2', $0xE9
    movq %rdi, __boot_info_ptr           # ③ 把 BootInfo* 存起来(存进 .data!)
    # 清 BSS
    movq $__bss_start, %rdi; movq $__bss_end, %rcx
    subq %rdi, %rcx; xorq %rax, %rax; rep stosb
    outb '3', $0xE9
    call _init_global_ctors              # ④ 跑全局构造
    outb '4', $0xE9
    movq __boot_info_ptr, %rdi           # 把 BootInfo* 作为参数
    call mini_kernel_main                # ⑤ 进 C++ main
```

这几行里其实藏着后面要讲的大坑(见"调试现场")。最要命的是第 ③ 步把 `BootInfo*` 存进 `__boot_info_ptr`,而这个变量放在 `.data` 段、不是 `.bss`——这点很关键,因为 `.bss` 紧接着会被清零,要是存进了 `.bss`,清零动作会把刚存的指针抹掉,后面 main 读到的就是 0,这正是"boot_info 损坏"的根因。另一个顺序约束是清 BSS 必须在跑全局构造之前:`.bss` 里是未初始化的全局/静态变量,C/C++ 语义要求它们启动时为 0,不清零全局对象的状态就是随机的。而全局构造(`_init_global_ctors`)本身又必须在 `main` 之前跑完——C++ 的全局对象(比如 `main.cpp` 里的 `global_counter`)的构造函数得在 `main` 之前执行,这是 C++ 运行时的规矩。

### 5. crt_stub.cpp:裸机 C++ 要自己带哪些运行时

普通 C++ 程序里,清 BSS、跑全局构造、`__cxa_pure_virtual`、`operator new/delete` 这些都由 libc/libstdc++ 的启动代码(crt0 等)和运行时库包办。我们用 `-nostdlib -ffreestanding` 编译内核,这些全没了,得自己补——这就是 [crt_stub.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/arch/x86_64/crt_stub.cpp) 的职责:

```cpp
// 遍历 .init_array,逐个调用全局构造函数
extern void (*__init_array_start[])();
extern void (*__init_array_end[])();
void _init_global_ctors() {
    for (void (**f)() = __init_array_start; f != __init_array_end; f++)
        (*f)();
}

// 这些要么不该被调用、要么我们还没实现,统一 hlt
[[noreturn]] void __cxa_pure_virtual() { while(1) asm("cli;hlt"); }
void* operator new(unsigned long)       { while(1) asm("cli;hlt"); }
void operator delete(void*) noexcept    { while(1) asm("cli;hlt"); }
// ... __stack_chk_fail、__cxa_atexit、operator new[]/delete[] 同理
```

`__init_array_start`/`__init_array_end` 是链接脚本在 `.init_array` 段前后打的符号,编译器把每个全局对象的构造函数指针放进这个段。遍历它、逐个调用,就是"跑全局构造"的全部实现。

`operator new/delete` 之所以写成"调到就 `hlt`":这一章**还没有堆**,但 C++ 的某些特性(比如带虚析构的类)会让链接器需要这些符号。我们提供"调到就死"的桩,既满足链接器,又确保谁要是真去 new 一个对象,立刻原地停下暴露问题,而不是悄悄跑飞。

### 6. main.cpp:用一组 C++ 冒烟测试自证运行时正常

内核的 `main`——[main.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/main.cpp) 的 `mini_kernel_main`——这一章不做什么"内核服务",而是用一组 C++ 测试来证明上面的运行时都对了:

- 一个普通类 `SimpleClass`,验证构造函数跑(`C1`)、成员正常;
- 一对带虚函数的基类/派生类(`Base`/`Derived`),验证**虚函数表(vtable)和动态派发**能工作(`V`、`2`);
- 一个全局对象 `global_counter`,验证**全局构造**在 main 前被调用(`G`、`3`);
- 最后校验 `BootInfo` 的 `entry_point`/`kernel_phys_base` 是不是预期的值(`B`)。

这套测试非常精明:它专门挑了"只有在 C++ 运行时正确初始化后才可能通过"的特性——虚函数(vtable 地址正确)、全局构造(.init_array 遍历对)、BootInfo 交接(rdi/.data 没被清零)。任何一环(清 BSS、全局构造、BootInfo 存储、高半映射)出问题,对应的标记就印不出来。看到 `===CPP … 1 2 3 B … ===END`,就等于这张运行时体检报告全绿。
