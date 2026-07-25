---
title: 04 · 调试现场:重构激活了潜伏的虚拟地址碰撞
---

# 调试现场:重构激活了潜伏的虚拟地址碰撞

## 调试现场:重构激活了一个潜伏的虚拟地址碰撞

模型照着改完了,一跑——`ext2 mount` 失败,报 `Port 1: command timeout`。可是 AHCI 初始化阶段分明检测到了 Port 1 的设备(`SSTS=0x113 DET=3`)。设备刚才还在,挂载时却超时了。开始剥。

### 第一个假设:调度器抢占打断了 AHCI DMA 轮询(红鲱鱼)

最直觉的怀疑:现在有抢占和时钟中断了,会不会是 AHCI 那套 DMA 轮询被中断打断、白白耗掉了超时时间?于是给 ext2 挂载外加 `InterruptGuard` 关中断试——**问题依旧**。假设排除。这一步很重要:别因为「听起来合理」就当真,得用实验证伪。

### 根因:内核栈的虚拟地址,盖住了 AHCI 的 MMIO

在 init 线程里打印 Port 1 的 SSTS 寄存器,现象一目了然:

```text
[AHCI] Port 1: SSTS=0x113 DET=3 SIG=0xffffffff   ← AHCI init 时，设备在线
[INIT] Port 1 SSTS=0x0 before mount                ← init 线程里，设备“消失”了
```

SSTS 从 `0x113` 掉成了 `0x0`。AHCI 的寄存器是 **MMIO**(memory-mapped I/O,映射在内存地址上的设备寄存器),读出来全零,几乎只有一个意思:**这块虚拟地址背后的页表映射被人覆盖了**。CPU 没在读设备,它在读一段被改写过的、指向别处的(或清零的)页表项。

那么谁覆盖了它?两个数字一对就破案了:

```text
AHCI MMIO 虚拟基址 (ahci.cpp, 008):   0xFFFF800000100000
内核栈虚拟起始     (process.cpp, 008): 0xFFFF800000100000   ← 完全相同
```

`TaskBuilder::build()` 每建一个 task,就分配 4 页内核栈,通过 `g_vmm.map()` 映射到 `next_stack_vaddr` 起始的虚拟地址往上长。而那个起始地址,跟 AHCI BAR5 的 MMIO 基址**一字不差**。于是:

```text
Scheduler::init() 建 idle task  → 栈映到 0x...100000 → 盖掉 AHCI MMIO
TaskBuilder 建 kernel_init      → 栈映到 0x...104000 → 盖掉 AHCI cmdlist/fis
TaskBuilder 建 boot            → 栈映到 0x...108000 → 进一步盖
```

第一个栈就把设备寄存器页表项冲了,后面 kernel_init 跑到 `ext2.mount()` 时,MMIO 早就坏了,自然超时。

### 为什么前几个 tag 从没暴露:重排顺序激活了潜伏 bug

这是这次排错最值得带走的一点。这个碰撞在代码里**早就存在**(两个魔法地址从它们各自被写进去那天起就相等了),为什么前面一直没事?

因为**执行顺序**。008 时,`Scheduler::init()`(也就是第一次映射内核栈的那一刻)发生在 `run_concurrent_stress()` 里;而 `ext2.mount()` 发生在 `kernel_main` 里、`run_concurrent_stress()` **之前**。也就是说:**挂载完成时,调度器还没启动,还没有任何内核栈被映射,MMIO 区域完好无损**。挂载一结束,后面才轮到调度器建栈——那时栈盖掉 MMIO 也无所谓了,因为已经没人再读它。

009 的重构把顺序换了:`Scheduler::init()` 提到 `kernel_main` 里、`ext2.mount()` 挪进 `kernel_init` 线程(在调度器启动**之后**)。于是「建栈」发生在「挂载」之前,潜伏的碰撞被**激活**了。

> 教训一:两个模块各自挑了一个「看起来很高半、互不相干」的虚拟地址,谁也没跟谁打招呼。散落在各文件里的硬编码虚拟地址,是定时炸弹——它们之间有没有重叠,没有任何一处代码能告诉你。
>
> 教训二:**重排执行顺序,会把「数据上的冲突」从潜伏变成发作**。这次是栈 vs MMIO;下次可能是两段 DMA 缓冲、或堆和页表。重构本身没写错任何新逻辑,它只是让一个早就成立的冲突,第一次在「错误的时间」被读到。

### 修复:统一内核虚拟内存布局

根因是「地址各写各的」,那就把它们集中到一个地方管。新建 `kernel/arch/x86_64/memory_layout.hpp`,把内核高半(`0xFFFF8000_00000000+`)划成首尾相接的区段,每段 `(base, size)`,下一段的 base 就是上一段的 `base + size`:

```text
KMEM_BASE = 0xFFFF800000000000
  Heap      [0x...000000, 0x...100000)   1 MB
  MMIO      [0x...100000, 0x...140000)   256 KB   (AHCI BAR5 等)
  Stack     [0x...140000, 0x...240000)   ↑ 每个 task 4 页，往上长
  DMA       [0x...240000, 0x...340000)   1 MB     (扇区读等)
  ext2 DMA  [0x...340000, 0x...440000)   1 MB     (ext2 块缓存)
```

(区段大小取自源码 `memory_layout.hpp` 的常量:`KMEM_MMIO_SIZE = 0x40000`,所以栈落在 `0x...140000`——MMIO 之后,不再重叠。)

然后把原先散落的三个魔法地址,全部改成引用这里的常量:

```diff
- static constexpr uint64_t MMIO_VIRT_BASE    = 0xFFFF800000100000ULL;
+ static constexpr uint64_t MMIO_VIRT_BASE    = cinux::arch::KMEM_MMIO_BASE;
- std::atomic<uint64_t> next_stack_vaddr{0xFFFF800000100000ULL};
+ std::atomic<uint64_t> next_stack_vaddr{cinux::arch::KMEM_STACK_BASE};
- static constexpr uint64_t EXT2_DMA_VIRT_BASE = 0xFFFF800000400000ULL;
+ static constexpr uint64_t EXT2_DMA_VIRT_BASE = cinux::arch::KMEM_EXT2_DMA_BASE;
```

以后想加一段新区域(比如后面要用的帧缓冲区、DMA 池),只要在布局表里插一行、后面的 base 自动顺延,不会再有「两个模块偷偷挑了同一个地址」的事。这次排错最终交付的,不只是「修好挂载」,而是一个**让这类冲突不再可能悄悄发生**的布局纪律。
