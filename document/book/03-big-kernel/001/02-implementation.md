---
title: 02 · 代码路线:big kernel 树与两阶段加载
---

# 代码路线:big kernel 树与两阶段加载

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

[boot.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/boot.S) 是 big kernel 的入口汇编,做的事和 [004](../01-boot/004/) mini kernel 的 `boot.S` 几乎一模一样——因为一个刚被加载进来的内核,开场动作永远是那几样:

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

头一条 `cli` 值得说一句:big kernel 跳进来时,脚下的段、分页、长模式都是 mini kernel 留下的,**它自己还没有 IDT**(建自己的 GDT/IDT 是 [002](../002/) 的事)。所以它必须先 `cli`,否则一个异步中断进来没人接,三重故障。这和 [002](../01-boot/002/) 当年的处境是同一个道理。

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

看到这行,意味着一整条链全通了:mini kernel 的 ATA 读盘、两阶段加载、重叠检查通过、ELF 段加载正确、物理跳转落点准、big kernel 的 boot.S 开场顺、它自己的串口和 kprintf 也工作。任何一个环节错,这行都打不出来——它是一个"全链路自检"的通过信号。从 [001](../01-boot/001/) 的 MBR 到这里,中间隔了八个 milestone,这一行是它们的共同终点。
