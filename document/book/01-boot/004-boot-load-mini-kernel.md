---
title: 004 · 加载第一个内核
---

# 004 · 加载第一个内核:从 bootloader 跳进 C++

> 前三章我们把机器从 MBR 一路抬到了 64 位长模式,但严格说,我们写的还都叫"bootloader"——一大堆汇编,没有一行真正的"内核代码"。这一章是 boot 卷的收尾:我们要把第一个**用 C++ 写的内核**从磁盘读进内存,通过一个交接结构把启动信息交给它,然后跳进去。从此,汇编 bootloader 的历史使命完成,接力棒交给 C++。

## 这一章我们要点亮什么

这一章的成果,是一台机器能跑到这个程度:

```text
... 长模式就绪(同 003) ...
 └─▶ 实模式收尾(还在 BIOS 能用的时候):
      ├─ query_memory_map()      # E820 查物理内存图 → 0x5000
      └─ load_kernel_from_disk() # 把 C++ 内核 ELF 读进物理 0x20000
 └─▶ 进长模式后(long_mode_entry):
      ├─ 填一张 BootInfo@0x7000(帧缓冲/内存图/入口地址)
      ├─ outb 'J'                # 要跳了
      └─ rdi = 0x7000; jmp 0xFFFFFFFF80020000   # 跳进 C++ 内核
           └─▶ kernel _start:
                ├─ 设栈、清 BSS、跑全局构造
                └─ call mini_kernel_main(BootInfo*)
                     └─▶ 一组 C++ 冒烟测试(类/虚函数/全局对象)
                          + 校验 BootInfo 完整
                          → halt
```

完成后,`build/debug.log` 里会从 003 的 `PL` 长成一串:`P L J 1 2 3 G 4 ===CPP … B … ===END`。那个 `===CPP…===END` 中间的测试标记和 `B`(BootInfo 校验通过),就是"一个 C++ 内核真的跑起来了"的铁证。

## 为什么现在需要它

长模式只是把舞台搭好。到目前为止,我们所有的"逻辑"都是写在汇编 bootloader 里的——配 GDT、搭页表、切模式。这套东西能做的事很有限,而且汇编写到后面越来越难维护。我们真正想要的,是一个用 C++ 写的、有类、有虚函数、有全局对象、能被现代工具链编译的内核。

但 C++ 内核不能凭空跑起来,它需要 bootloader 替它做三件"一旦进 64 位就做不了"的事:

1. **把它从磁盘读进内存**(读盘要用 BIOS,只能在实模式做)。
2. **把它需要的启动信息收集好**(物理内存图要靠 BIOS 的 E820,帧缓冲参数是 001 配 VESA 时拿到的)。
3. **给它一个能落脚的地址和一份交接说明**(内核链接在高半地址,我们得在页表里把那个地址映射好,再用一个结构把信息传过去)。

这三件事,正好对应这一章的三个主角:**ELF 加载**、**BootInfo 交接**、**高半内核**。做完它们,bootloader 就可以功成身退了。

## 设计图

先看磁盘和内存两个布局。

**磁盘布局**(比 001 多了一段内核):

```text
扇区 0       MBR(512B)
扇区 1..15   Stage2(≤7.5KB)
扇区 16+     mini kernel ELF(416KB,832 扇区)
```

**内存布局**(004 新增/用到的关键地址):

```text
0x5000   E820 内存图(query_memory_map 写入)
0x6400   VESA 帧缓冲信息(001 存的)
0x7000   BootInfo 交接结构(824 字节,bootloader 填、内核读)
0x20000  mini kernel 物理载入地址(LMA)
0x90000  保护模式/长模式栈(内核加载要避开它——见调试现场)
0xFFFFFFFF80020000  mini kernel 虚拟运行地址(VMA,高半)
```

**调用链与交接**:

```text
bootloader(实模式): 读盘 → 内存图
   ↓
long_mode_entry(64 位): 填 BootInfo@0x7000 → rdi=0x7000 → jmp 高半入口
   ↓                          ↑ rdi 传参(System V AMD64 ABI)
kernel _start: 存 boot_info → 清 BSS → 全局构造 → main(BootInfo*)
```

## 代码路线

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

这几行里其实藏着后面要讲的大坑(见调试现场)。最要命的是第 ③ 步把 `BootInfo*` 存进 `__boot_info_ptr`,而这个变量放在 `.data` 段、不是 `.bss`——这点至关重要,因为 `.bss` 紧接着就会被清零,要是存进了 `.bss`,清零动作会把刚存的指针抹掉,后面 main 读到的就是 0,这正是"boot_info 损坏"的根因。另一个顺序约束是清 BSS 必须在跑全局构造之前:`.bss` 里是未初始化的全局/静态变量,C/C++ 语义要求它们启动时为 0,不清零全局对象的状态就是随机的。而全局构造(`_init_global_ctors`)本身又必须在 `main` 之前跑完——C++ 的全局对象(比如 `main.cpp` 里的 `global_counter`)的构造函数得在 `main` 之前执行,这是 C++ 运行时的规矩。

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

## 调试现场

004 的 A/B/C 三个 tag,本质上就是"让第一个内核跑起来"过程中踩的三个连环坑。这三个坑在源码注释里都留下了修复痕迹,是非常好的教材。

**坑一(004_A→B)**——内核加载和栈撞在一起。症状:内核刚加载、或一进保护模式就崩。根因:内核被读到了和"保护模式栈(0x90000)"重叠的区域,几层函数压栈就把内核代码盖掉了。修复:把内核载入地址定在更低的 `0x20000`,和栈 `0x90000` 之间留出足够 gap(stage2 那句注释 "leaving 32KB gap before protected mode stack at 0x90000" 就是这次修复的备忘)。教训:**低地址那片 1MB 是"兵家必争之地"——MBR、栈、BIOS 数据区、内核加载区全挤在这**,地址分配必须画清边界,谁也别踩谁。

**坑二(004_B→C)**——BootInfo 传过去就坏了。症状:内核跳进去了、main 也跑了,可一读 `BootInfo` 字段全是 0 或乱码(`B` 标记印不出来)。根因:早期版本把 `BootInfo*`(`rdi`)存进了一个 `.bss` 变量;而 `boot.S` 紧接着会清零整个 `.bss`——刚存的指针被抹成 0。修复:把 `__boot_info_ptr` 放到 **`.data` 段**(已初始化数据,不在清零范围内),并且"存指针"必须在"清 BSS 之前"。源码里那句 `/* Save BootInfo pointer BEFORE clearing BSS */` 就是这条血的教训。

**坑三(004_C)**——裸机 C++ 的符号冲突 / 链接失败。症状:加上带虚函数的类、全局对象后,链接器报 `undefined reference to __cxa_pure_virtual / operator delete / ...` 一堆错,或全局对象的构造没跑。根因:`-nostdlib` 砍掉了 C++ 运行时,但凡用到虚函数(需要 `__cxa_pure_virtual`)、虚析构(需要 `operator delete`)、全局对象(需要 `.init_array` 遍历)就会缺符号或行为不对。修复:写 `crt_stub.cpp` 补齐这些桩 + `_init_global_ctors`,并在链接脚本里正确导出 `__init_array_start/end`、`__bss_start/end`。教训:**裸机 C++ 不是"去掉 main 的普通 C++"**,你得自己把语言运行时那一层补回来。

## 验证

第一道闸还是构建。现在 image 由三段拼成:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -S .
cmake --build build -j$(nproc)
```

`build/kernel/mini/mini_kernel.bin`(以及 mbr.bin/stage2.bin)产出,说明内核这套 freestanding 编译、链接脚本、objcopy 全过了。

第二道闸看 debugcon 序列。`cmake --build build --target run`,看 `build/debug.log`,期望按序出现:

```text
P L J 1 2 3 G 4 ===CPP C1 1 V 2 3 B ===END
```

逐段对应:`P/L`=003 的 PM/长模式;`J`=bootloader 要跳了;`1/2/3`=内核 `_start` 前三步(到、设栈、清 BSS);`G`=全局对象 `global_counter` 的构造(由 `_init_global_ctors` 触发,**夹在 `3` 与 `4` 之间**);`4`=全局构造跑完;`===CPP…===END`=main 的 C++ 冒烟测试;中间的 `1/2/3`=三项测试通过、`B`=BootInfo 校验通过。少了哪一段,就照"调试现场"三个坑对号入座。

第三道闸用 GDB 确认跳进高半。`cmake --build build --target run-debug`:

```text
(gdb) file build/kernel/mini/mini_kernel      # 内核 ELF
(gdb) target remote :1234
(gdb) b *mini_kernel_main
(gdb) c
(gdb) p/x $rdi                # 应是 0x7000(BootInfo*)
(gdb) p/x $rip                # 应在 0xFFFFFFFF8002xxxx 高半
```

断在 `mini_kernel_main`、`rdi=0x7000`、`rip` 在高半,说明交接和跳转都对了。

## 下一站

boot 卷到这里收尾:从 MBR 到长模式、再到第一个 C++ 内核跑起来,整条引导链完整了。bootloader 的活干完了——但它交给内核的,还只是一个"能跑 C++、有一份启动信息"的空壳。内核现在没有内存管理、没有中断、没有进程,甚至连一块能 `new` 的堆都没有(operator new 调到就死)。

接下来是 [02-mini-kernel 卷](../02-mini-kernel/005-mini-kernel-entry.md):内核从 `mini_kernel_main` 开始真正接管机器——先给自己搭一套物理内存管理(PMM),再处理中断,把自己从一个"会跑 C++ 的空壳"变成一个"能管资源"的小内核。从那以后,主角就是内核自己了。

---

### 参考

- System V AMD64 ABI — 整型参数传递顺序(`%rdi` 为第一参数),BootInfo 交接的依据。
- OSDev — [Higher Half Kernel](https://wiki.osdev.org/Higher_Half_Kernel)(高半内核与临时双映射)、[ELF](https://wiki.osdev.org/ELF)(VMA/LMA、`AT()` 物理落点)、[Detecting Memory (x86): E820](https://wiki.osdev.org/Detecting_Memory_(x86)#e820)、[Calling Global Constructors](https://wiki.osdev.org/Calling_Global_Constructors)(`.init_array` 与裸机 C++ 运行时)、[C++ Bare Bones](https://wiki.osdev.org/C%2B%2B_Bare_Bones)(freestanding 标志、crt 桩)。
- 本 tag 源码:[boot.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/common/boot.S)、[boot_info.h](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/boot_info.h)、[stage2.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/stage2.S)(`long_mode_entry` 填 BootInfo 与跳转)、[long_mode.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/common/long_mode.S)(高半映射)、[linker.ld](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/linker.ld)、[boot.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/arch/x86_64/boot.S)、[crt_stub.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/arch/x86_64/crt_stub.cpp)、[main.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/main.cpp)、[build_image.sh](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/scripts/build_image.sh)。
- 调试素材提炼自 [kernel_load_stack_collision.md](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/document/notes/004-B/kernel_load_stack_collision.md)、[boot_info_param_corruption.md](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/document/notes/004-C/boot_info_param_corruption.md)、[bss_data_symbol_conflict.md](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/document/notes/004-C/bss_data_symbol_conflict.md)。

> Intel SDM 版本说明:本卷引用的 SDM 章节号沿用较早版本编号;若按项目本地 PDF(2023-06 版)查阅,内容位置以章节标题为准(System V AMD64 ABI、OSDev 的引用不受此影响)。

> 参考 URL 的有效性会在全局审查阶段用 open-websearch(bing)统一核活。
