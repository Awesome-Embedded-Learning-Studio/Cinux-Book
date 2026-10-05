---
title: 07 · 起居重排
description: "entry.cpp 从杂货铺瘦成日程表:清 .bss、LoadOwnedGdt、InstallExceptionStubs、LoadIdt、Main、Halt,一步一讲究——门里的 0x08 在 boot 的旧表里是 32 位代码段,通电次序错一步就是黑屏。面板添到五行。"
chapter: 8
order: 7
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
---

# 起居重排

家什都备齐了,该把它们排进日程了。出生那一卷攒下的 entry.cpp 是个杂货铺:五个手搓的汇编桩、安装函数、IDT 表本体、lidt,连带着扫院子的活,全挤在了一个文件里。本卷咱们给它瘦身,瘦成了一张日程表:手搓桩那一摊子搬去了 isr 家,异常表搬去了 idt 家,段表搬去了 gdt 家,entry.cpp 剩下的,是扫帚和排程的手。kernel/arch 这个目录里的三家各立了门户,从这一卷起算是立住了正经器官的名分。

咱们从 KernelEntry 顶上看起。出生那一卷它的四件事是立栈、存 rdi、自清 BSS、装异常,如今头一件换了人,开禁那四笔控制寄存器的读写占住了最顶上:

```cpp
asm volatile(
    "movq %%cr0, %%rax\n\t"
    "andq $~0x4, %%rax\n\t"
    "orq $0x2, %%rax\n\t"
    "movq %%rax, %%cr0\n\t"
    "movq %%cr4, %%rax\n\t"
    "orq $0x600, %%rax\n\t"
    "movq %%rax, %%cr4\n\t"
    "movabsq $0x90000, %%rsp\n\t"
    "xorq %%rbp, %%rbp"
    :
    :
    : "rax", "memory");
```

位在汇编里是裸的,咱们对着上一节念一遍:CR0 那三行,清的是 0x4 那粒 EM,置的是 0x2 那粒 MP。CR4 的那一笔 0x600,是 OSFXSR 和 OSXMMEXCPT 两粒一起的置位。然后栈立到了 0x90000,rbp 清了零。为什么开禁必须排头,上一节讲清了:任何一句 C++ 都可能带着 SSE,地基不能晚于头一个住户。这块汇编跑完了,rdi 里的交接单存好了,接力棒就交给了 save_handoff_and_start。咱们排的日程拢共六步,每一步都有它的讲究:

```cpp
memory_zero(g_kernel_bss_start, kBssBytes);
cinux::arch::gdt::LoadOwnedGdt();
cinux::arch::isr::InstallExceptionStubs();
cinux::arch::idt::LoadIdt();
kernel::Main(*cinux::base::PtrAt<cinux::boot::BootInfo>(info_addr));
cinux::console::Halt();
```

头一步清的是 .bss,次序是出生那一卷就立好的:表自己也是院子里的住户,扫院子的事在前、挂门牌的事在后,反了扫帚会把刚挂上的牌一并扫掉。第二步换的是段表,CS 从借来的 `0x18` 落到了自家的 `0x08`。第三步装的是桩,三十二桩进了门,门里记的选择子是 `0x08`。第四步是 lidt 的通电。夹在第三步和第四步之间的,是本卷最要紧的一次序讲究,咱们细看这一步:lidt 一执行,CPU 从此被打断就认自家的表,异常是随时可能进来的。异常进了门,CPU 拿门里记的 `0x08`,去当前生效的 GDT 里找段。在咱们自己的表里,`0x08` 记的是 64 位代码段,而要是段表还没换,当前生效的还是 boot 的旧表,`0x18` 才是 64 位的代码,轮到 `0x08` 记的,却是 32 位的!异常一进来,桩就跑在了 32 位语法里,当场又是另一重故障了,连锁下去就是三重故障的黑屏。所以段表必须换在前、桩必须装在前、lidt 必须最后通电:表空着就上电,是比不通电还糟的。第五步里 Main 正主进了场,第六步 Halt 收了尾。

kernel.cpp 那头跟着动了一行:面板第二行新添了 `gdt+idt self-owned, sse on`,本卷三件事的自我报告,一行就打齐了。名册那头的 Pmm,咱们这一卷顺手立成了自家的单例:Pmm::self() 是拿同一个名册实例的门把手,init 吃的也换成了整张交接单。一家一户的日子,从这就过稳了。

咱们把起居排完了,面板也从四行变到了五行——boot 的老行还在前头,这里数的是内核自家报的那五行。开机看什么、每一笔数怎么对得上,还有一针真的疼——都到验收那天见。
