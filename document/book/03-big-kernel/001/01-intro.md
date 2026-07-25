---
title: 01 · 导引:点亮什么、为什么、设计图
---

# 导引:点亮什么、为什么、设计图

## 这一章我们要点亮什么

两件事同时发生:一个新内核诞生,另一个内核功成身退。

big kernel 是一个全新的源码树 `kernel/`(和一直以来的 `kernel/mini/` 并列)。它有自己的入口汇编、自己的运行时桩、自己的串口和 kprintf——简单说,它得像 [004](../01-boot/004/) 的 mini kernel 当年那样,从零把自己跑起来。这一章它的 `main` 只做一件最有仪式感的事:初始化串口、打印 `[BIG] Big kernel running @ 0x1000000`、然后停下。一句话,但这句话证明的是——一个被从磁盘加载进来的、独立的 C++ 内核,真的在 16MB 那个地址跑起来了。

mini kernel 这边则把 [004](../02-mini-kernel/004/) 那个"只 demo、没真用"的加载器升级成真能用:把加载拆成"先读头部探明大小、再读整份、加载段"的两阶段,在加载前做一次内存布局重叠检查(防止把正在跑的 mini kernel 自己或页表覆盖掉),并把分页的恒等映射按需扩展。它的 `main` 现在真的会调用 `load_big_kernel`、拿到入口地址、`jmp` 过去——这一跳之后,mini kernel 的代码就再也不执行了,舞台完全交给 big kernel。

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
