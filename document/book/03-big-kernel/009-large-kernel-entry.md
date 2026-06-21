---
title: 009 · 大内核登场
---

# 009 · 大内核登场:mini kernel 交棒

> 从 [004](../01-boot/004-boot-load-mini-kernel.md) 诞生起,mini kernel 的全部意义就是一件事:把那个功能完整的 **big kernel** 弄进内存、把自己手里的控制权交出去。前四章(005–008)它把自己收拾利索——会输出、有内存、能扛异常、造好了读盘和 ELF 加载的家伙——就等这一刻。这一章,big kernel 终于登场:mini kernel 用升级过的加载器把它从磁盘读进来、跳进它的入口。串口上那一行 `[BIG] Big kernel running @ 0x1000000`,就是整个 004–009 接力跑的终点信号。

## 这一章我们要点亮什么

两件事同时发生:一个新内核诞生,另一个内核功成身退。

big kernel 是一个全新的源码树 `kernel/`(和一直以来的 `kernel/mini/` 并列)。它有自己的入口汇编、自己的运行时桩、自己的串口和 kprintf——简单说,它得像 [004](../01-boot/004-boot-load-mini-kernel.md) 的 mini kernel 当年那样,从零把自己跑起来。这一章它的 `main` 只做一件最有仪式感的事:初始化串口、打印 `[BIG] Big kernel running @ 0x1000000`、然后停下。一句话,但这句话证明的是——一个被从磁盘加载进来的、独立的 C++ 内核,真的在 16MB 那个地址跑起来了。

mini kernel 这边则把 [008](../02-mini-kernel/008-load-large-kernel.md) 那个"只 demo、没真用"的加载器升级成真能用:把加载拆成"先读头部探明大小、再读整份、加载段"的两阶段,在加载前做一次内存布局重叠检查(防止把正在跑的 mini kernel 自己或页表覆盖掉),并把分页的恒等映射按需扩展。它的 `main` 现在真的会调用 `load_big_kernel`、拿到入口地址、`jmp` 过去——这一跳之后,mini kernel 的代码就再也不执行了,舞台完全交给 big kernel。

## 为什么现在需要它

mini kernel 其实是个"跳板内核"。它存在,不是为了自己当主角,而是因为 x86 的上电流程太复杂(BIOS、实模式、保护模式、长模式、读盘、解析 ELF),我们没法在 bootloader 那一小段汇编里一口气搞定。于是 Cinux 的策略是分两层:汇编 bootloader 把 mini kernel 弄起来,mini kernel 再把 big kernel 弄起来。008 已经把"弄起来"的家伙造好、也用 demo 验过它们能干活了——但当时盘上还没有 big kernel,那杆枪没靶子可打。

009 就是靶子竖起来的那一刻。big kernel 一旦存在、被加载、能跑,mini kernel 的全部使命就完成了。从这以后,我们写的一切新功能(驱动、进程、文件系统、GUI)都加在 big kernel 里,mini kernel 冻结在"加载器"这个角色上,不再演进。

为什么 big kernel 要单开一个树、而不是接着在 mini kernel 里写?因为它们的定位完全不同。mini kernel 追求极简(越少越好,只为加载服务),用裸二进制;big kernel 是正式内核,用标准 ELF、会越长越大。把两者分开,各自用最适合它的形式,互不拖累。

> 外部依据:OSDev 的 Higher Half Kernel 页讨论了内核运行在高半地址、由一个 loader 加载并跳转的常见架构;ELF 规范定义了 PT_LOAD 段的 p_offset/p_paddr 与加载语义。

## 设计图

先看这两套内核和磁盘的关系。到现在盘上住了四房客:

```text
扇区 0        MBR
扇区 1..15    Stage2
扇区 16..847  mini kernel(004 加载、flat binary)
扇区 848+     big kernel(009 入住,标准 ELF)
```

再看交棒的完整旅程——mini kernel 的最后几步,接上 big kernel 的头几步。加载是**两阶段**的,精髓是"先探明这内核到底多大,再按需读、按需映射":

```text
mini kernel main(长模式,有 PMM/GDT/IDT/ATA/ELF loader):
   load_big_kernel(LBA 848):
     Phase 1: 只读 ELF 头那几个扇区 → 验 magic、解析 program header、算出整份 ELF 多大
     Phase 2: 按算出的大小扩展恒等映射 → 重叠检查 → 读完整 ELF 到 staging@0x1000000
              → load_elf:拷 PT_LOAD 段到 p_paddr、清 BSS → 返回入口(物理)
   jmp <入口>   ← 交棒(跳的是物理地址,见下文)
        └─▶ big kernel _start (boot.S):
             ├─ cli、设栈、清 BSS、全局构造
             └─ call kernel_main
                  └─ kprintf_init() + 打印 [BIG] ... @ 0x1000000 + halt
```

这里有个和直觉不太一样的点:big kernel 在 ELF 头里声明的入口(`e_entry`)是高半虚拟地址 `0xFFFFFFFF81000000`,但 `load_elf` 返回入口前会把它**减去高半基址**、换算成物理 `0x1000000`,mini kernel 实际 `jmp` 的是这个**物理地址**(靠恒等映射落地)。换句话说,这一跳走的是物理/恒等那条路,不依赖高半映射命中。这和"内核最终要在高半跑"不矛盾——只是 009 这个极简阶段,先在物理地址把它跑起来就够了,高半留待内核自己重建页表后再用。

## 代码路线

### 1. big kernel 长什么样

big kernel 是个独立的 `kernel/` 树,结构和当年的 mini kernel 如出一辙,只是更"正式":标准 ELF 镜像、有自己的运行时。它的 [main.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/main.cpp) 极简:

```cpp
extern "C" void kernel_main() {
    cinux::lib::kprintf_init();
    cinux::lib::kprintf("[BIG] Big kernel running @ 0x1000000\n");
    while (1) __asm__ volatile("cli; hlt");
}
```

`kprintf_init` 先把串口(COM1)初始化好——big kernel 不能假设 mini kernel 留下的串口状态,它得自己把自己的输出通道建起来。然后那行 `[BIG] ...`,就是"我到了"的信号。

### 2. boot.S:和 mini kernel 一样的开场

[boot.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/boot.S) 是 big kernel 的入口汇编,做的事和 [004](../01-boot/004-boot-load-mini-kernel.md) mini kernel 的 `boot.S` 几乎一模一样——因为一个刚被加载进来的内核,开场动作永远是那几样:

```asm
_start:
    cli                              # 还没有自己的 IDT,先关中断
    movq $__kernel_stack_top, %rsp   # 自己的栈(linker.ld 给的 16KB)
    # 清 BSS
    movq $__bss_start, %rdi; movq $__bss_end, %rcx
    subq %rdi, %rcx; xorq %rax, %rax; rep stosb
    call _init_global_ctors          # 跑全局构造
    xorq %rdi, %rdi                  # BootInfo* = NULL(暂时)
    call kernel_main
```

头一条 `cli` 值得说一句:big kernel 跳进来时,脚下的段、分页、长模式都是 mini kernel 留下的,**它自己还没有 IDT**(建自己的 GDT/IDT 是 [010](010-big-kernel-gdt.md) 的事)。所以它必须先 `cli`,否则一个异步中断进来没人接,三重故障。这和 [002](../01-boot/002-boot-gdt-protected.md) 当年的处境是同一个道理。

注意 `kernel_main` 这里收的 `BootInfo*` 是 NULL。原因是清 BSS 的 `rep stosb` 用了 `%rdi`,把 mini kernel 跳转时传进来的 `BootInfo*`(按 System V ABI 在 `%rdi`)给冲掉了。009 先不处理这个——big kernel 这章根本不用 BootInfo。源码里那句 TODO("如果需要 BootInfo,清 BSS 前先存 %rdi")就是留给后面补的。

### 3. mini kernel 的 loader 升级:两阶段加载 + 重叠检查

008 的 `load_big_kernel` 是个能跑但没被调用的 demo。009 把它升级成真家伙(见 [big_kernel_loader.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/big_kernel_loader.cpp)),并在 mini kernel 的 main 里真正调用。升级的思路是**两阶段**和**别把不该覆盖的东西覆盖了**。

为什么分两阶段?因为加载前 mini kernel 不知道这份 big kernel 到底多大——它只预订了一个上界。于是 Phase 1 先只读 ELF 头那几个扇区,验 magic、把 program header 解析进一个**局部数组**、由各段的 `p_offset+p_filesz` 算出整份 ELF 的真实大小。知道大小后,Phase 2 才按需扩展恒等映射、读完整 ELF、加载段。这样既能装任意大小的内核,又不会盲目映射/读取过多。

Phase 2 在真正动数据前,还做一次**内存布局重叠检查**——把页表区(`0x1000–0x4000`)、mini kernel 自身(`0x20000` 起)、以及 big kernel 各 PT_LOAD 段的目标区 (`p_paddr`) 登记成一组区域,两两检查有没有重叠。一旦发现"big kernel 的段要落到 mini kernel 还在跑的代码上"这种事,立刻中止加载,而不是闷头 `memcpy` 把自己覆盖掉、落得个三重故障。注意 staging 缓冲区(`0x1000000`)**故意不**登记进重叠检查——因为按设计 big kernel 就是 load-in-place(staging 和 PT_LOAD 目标同址),这是允许的;真正要拦的是"打到 mini kernel/页表"那种致命重叠。这个运行时检查,加上构建期辅助的 `scripts/check_memory_layout.py`,把加载器的安全性兜住了。

### 4. 跳过去:物理地址、恒等映射

加载完,`load_elf` 返回入口。这里有个容易想当然的细节:ELF 头里的 `e_entry` 是高半虚拟地址 `0xFFFFFFFF81000000`,但 `load_elf` 在返回前判断了一下——如果入口落在高半,就把它**减去 `0xFFFFFFFF80000000`、换算成物理 `0x1000000`** 再返回。于是 mini kernel `jmp` 的是物理 `0x1000000`,靠 mini kernel 的**恒等映射**落地进 big kernel 的 `_start`。

那为什么 009 还要动分页([paging.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/arch/x86_64/paging.hpp))?不是为高半跳转(那条路这章没走),而是因为 big kernel 可能很大、要落到比当前恒等映射更高的物理地址。Phase 2 用 `identity_map_up_to(highest_phys)` 把恒等映射按需往高处扩,确保 big kernel 落点那些物理地址都能正常访问。至于高半虚拟地址,bootloader 早在进长模式时就顺手把 `PML4[511]` 指向了同一套页表,所以高半那条路本来也是通的——只是 009 的物理跳转用不到它罢了。

### 5. kernel_main:kprintf_init + 打印 [BIG]

跳进 big kernel、走完 boot.S 的开场,最后落到 `kernel_main`。它 `kprintf_init` 把自己的串口建好,然后那行 `[BIG] Big kernel running @ 0x1000000` 就打到串口上了。

看到这行,意味着一整条链全通了:mini kernel 的 ATA 读盘、两阶段加载、重叠检查通过、ELF 段加载正确、物理跳转落点准、big kernel 的 boot.S 开场顺、它自己的串口和 kprintf 也工作。任何一个环节错,这行都打不出来——它是一个"全链路自检"的通过信号。从 [001](../01-boot/001-boot-real-mode.md) 的 MBR 到这里,中间隔了八个 milestone,这一行是它们的共同终点。

## 调试现场

这一章留下一个特别精彩的 bug,记录在 [009-01-elf-loader-header-corruption.md](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/document/notes/009/009-01-elf-loader-header-corruption.md)。

症状是:`test_big_kernel_load` 跑到打印 `PT_LOAD[0]` 信息后突然中断,QEMU 直接退出,连个异常都没有,后面所有测试消失。崩溃点卡在 ELF 加载的"打印段信息"和"Loaded segment"两行之间——也就是 `load_elf` 拷段的那一刻。

根因是**加载器把自己正在读的 ELF 头覆盖了**。big kernel 的 staging 缓冲区在 `0x1000000`,而它的 `PT_LOAD[0].p_paddr` 也是 `0x1000000`——同一个地址。`load_elf` 一边从 staging 里读 ELF 头和 program header(在镜像头部),一边把段数据 `memcpy` 到 `p_paddr`(也就是 staging 起点)。这一拷,段数据直接盖到了 ELF 头上;循环到下一段再去读 program header,读到的已经是段数据的垃圾,当场崩。

这是个"load-in-place"布局下的经典陷阱:当目标物理地址和 staging 地址重合,`memcpy` 的源和目的区域会重叠,既不能用 `memcpy`(重叠区是未定义行为),又不能用普通的"边读头边加载"。Cinux 的修法分两步:其一,`load_elf` 在动数据前先把 program header 整组**快照到一个本地数组**(`saved_phdrs`),后续循环读的是快照、不再碰 staging 里会被覆盖的那份;其二,段拷贝改用 `memmove`(重叠区安全)而不是 `memcpy`。再加上前面说的运行时 `check_memory_overlaps` 和构建期 `check_memory_layout.py` 把致命重叠挡在加载前。这个 bug 现在是"过去时",但它的教训——**加载器的源区(staging)和目的区(p_paddr)一旦可能重叠,就得先快照元数据、再用 memmove**——值得记一辈子。

## 验证

这一章的验证是"全链路"的——从 mini kernel 一路看到 big kernel 的那行字。

构建(注意现在 image 要包含 big kernel):

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -S .
cmake --build build -j$(nproc)
```

host 单测验加载器逻辑:

```bash
cmake --build build --target test_host
```

QEMU 内核测验"真加载"和"压力":

```bash
cmake --build build --target run-kernel-test
```

`test_big_kernel_load` 验真加载流程通(顺带用它附带的 CRC32 断言核对镜像完整性——注意 CRC32 在 Cinux 里是**测试里的独立断言**,不是生产加载路径上的关卡),`test_stress_big_kernel` 拿 1GB 合成 ELF 压 loader。

量产看交棒那一刻:

```bash
cmake --build build --target run
```

串口上先是 mini kernel 一路走完它 005–008 的所有输出,然后加载器的 Phase 1 打印 `[LOADER] Phase 1: Reading ... sectors from LBA ...`、`[LOADER] ELF file: ... bytes (... sectors)`;Phase 2 打印 `Mapping physical memory up to ...`、内存布局表 `[OK] No overlaps detected.`、`[LOADER] Phase 2: Reading ... sectors from disk...`、`[LOADER] Big kernel loaded successfully.`、`[LOADER] Entry point: 0x1000000`。紧接着 mini kernel 跳转,big kernel 的串口出现那行决定性的 **`[BIG] Big kernel running @ 0x1000000`**,然后安静停下。这一行,就是 009 的通过信号,也是整个 mini-kernel 卷的句号。

## 下一站

big kernel 现在能跑了,但它脚下踩的还是 mini kernel 留下的"临时基建"——mini kernel 的 GDT、mini kernel 的临时分页。它自己还没有 IDT,所以全程 `cli`,任何异常一来就三重故障。一个不能被中断、不能扛异常的内核,没法继续往上堆驱动和进程。

下一章 [010 · 大内核的 GDT](010-big-kernel-gdt.md),big kernel 要建它自己正式的 GDT(带 TSS、为后面的特权级和中断栈切换准备),把 mini kernel 留下的临时段表换掉。从那以后,big kernel 才算真正"站稳",开始按自己的规矩运行。

---

### 参考

- OSDev — [Higher Half Kernel](https://wiki.osdev.org/Higher_Half_Kernel)(高半加载与跳转架构)、[ELF](https://wiki.osdev.org/ELF)(PT_LOAD、p_offset/p_paddr、加载语义)。
- ELF64 / TIS 规范 — 程序头字段、PT_LOAD 段、`memcpy` 与重叠区(`memmove`)语义。
- 本 tag 源码:[main.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/main.cpp)、[boot.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/boot.S)、[kprintf.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/lib/kprintf.cpp)、[big_kernel_loader.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/big_kernel_loader.cpp)(两阶段加载 + 重叠检查)、[elf_loader.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/elf_loader.cpp)(快照 phdr + memmove + 物理入口换算)、[paging.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/arch/x86_64/paging.hpp)(`identity_map_up_to`)、[check_memory_layout.py](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/scripts/check_memory_layout.py)、[generate_large_elf.py](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/scripts/generate_large_elf.py)。
- 调试素材提炼自 [009-01-elf-loader-header-corruption.md](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/document/notes/009/009-01-elf-loader-header-corruption.md)。

> 参考 URL 的有效性会在全局审查阶段用 open-websearch(bing)统一核活。
