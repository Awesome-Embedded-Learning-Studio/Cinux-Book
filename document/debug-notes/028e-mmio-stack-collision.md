---
title: Debug · init 线程化炸出的虚拟地址碰撞:栈盖住了 MMIO
---

# Debug · init 线程化炸出的虚拟地址碰撞:栈盖住了 MMIO

> 出处:tag `028e_activate_init_thread`,`document/notes/028e/001_init_thread_refactor_mmio_collision.md`。这里按「症状 → 定位 → 根因 → 修复 → 防复发」提炼,不照抄原始笔记。地址数字以 tag 源码 `memory_layout.hpp` 为准。

## 症状

把启动流程重构成 init 线程模型(boot task + `kernel_init` 线程接管 FS/shell)之后,一启动就炸:

```text
[AHCI] Port 1: SSTS=0x113 DET=3 SIG=0xffffffff   ← AHCI init 阶段，设备明明在线
...
[INIT] ext2 mount failed!                         ← 挂载失败
[AHCI] Port 1: command timeout                    ← 报 command timeout
```

诡异之处在于:AHCI 初始化时 Port 1 检测到设备(`SSTS=0x113 DET=3`),可一进 init 线程、还没怎么动,挂载就超时了。设备「刚才还在,转眼没了」。

## 定位

**第一个假设:调度器抢占打断了 AHCI 的 DMA 轮询。** 这是重构后最直觉的嫌疑人——现在有抢占和 100 Hz 时钟中断了,会不会是 AHCI 那套「发命令 → 轮询完成」被打断、白白耗掉了超时?于是给 ext2 挂载外包一层 `InterruptGuard` 关中断再试。**问题依旧**。假设排除。

> 排错第一守则:别因为「听起来合理」就当真。「有中断了 → 一定是中断打断的」是典型的想当然,得用实验证伪。这一步省不得,否则会一路错下去。

**第二个动作:在 init 线程里直接读 Port 1 的 SSTS 寄存器。** 一读,真相浮出水面:

```text
[AHCI] Port 1: SSTS=0x113 DET=3 SIG=0xffffffff   ← AHCI init 时
[INIT] Port 1 SSTS=0x0 before mount                ← init 线程里，变成 0
```

`SSTS` 从 `0x113` 掉成了 `0x0`。AHCI 的寄存器是 **MMIO**——映射在内存地址上的设备寄存器,通过普通 `mov` 读写。一个设备寄存器读出全零,且它的值会「无故变化」,几乎只有一个解释:**这块虚拟地址背后的页表映射被人改写了**,CPU 根本没在读设备,它在读一段指向别处(或被清零)的页表项。

**第三步:找谁改了它。** 两个魔法地址一对,当场破案:

```text
AHCI MMIO 虚拟基址 (ahci.cpp):     0xFFFF800000100000
内核栈虚拟起始   (process.cpp):    0xFFFF800000100000   ← 完全相同
```

`TaskBuilder::build()` 每建一个 task,就分配 4 页内核栈,经 `g_vmm.map()` 映射到 `next_stack_vaddr` 往上长。而这个起始地址,跟 AHCI BAR5 的 MMIO 基址**一字不差**。建栈的过程就是在 MMIO 区域上「盖楼」:

```text
Scheduler::init() 建 idle task → 栈 0x...100000 → 盖掉 AHCI BAR5 MMIO
TaskBuilder 建 kernel_init      → 栈 0x...104000 → 盖掉 cmdlist/fis
TaskBuilder 建 boot            → 栈 0x...108000 → 进一步盖
```

## 根因

两个数字相等不是巧合,是**两个模块各自挑了同一个高半地址、谁也没告诉谁**。AHCI 驱动自己定了个 `MMIO_VIRT_BASE = 0xFFFF800000100000` 用来映射 BAR5;调度器的 `next_stack_vaddr` 也从 `0xFFFF800000100000` 起步。散落在不同文件里的硬编码虚拟地址,没有任何一处代码会去检查它们之间有没有重叠。

那为什么前面几个 tag 一直没事?这是这次最值得想通的一点——**重排执行顺序,把潜伏的冲突激活了**。

```text
028d 的顺序(碰撞潜伏):
   kernel_main: AHCI init → ext2.mount()  ← 挂载先完成，此时调度器还没起、没有内核栈
              → run_concurrent_stress()    ← 调度器在这里才 init，建第一个栈(盖 MMIO)
                                          ← 但挂载早结束了，盖了也无所谓

028e 的顺序(碰撞发作):
   kernel_main: AHCI init → Scheduler::init()  ← 调度器提前，建 idle 栈(盖掉 MMIO)
              → spawn kernel_init 线程
              → kernel_init 里 ext2.mount()      ← 挂载在「栈已盖 MMIO」之后，读到的全是 0
```

碰撞在代码里**早就成立**(两个地址从各自被写进源码那天起就相等),只是 028d 的顺序里,「读 MMIO」全部发生在「第一次建栈」之前,所以一直没被读到错的值。重构没写错任何新逻辑,它只是把「建栈」挪到了「挂载」之前,让这个早就存在的冲突第一次在错误的时间被访问。

## 修复

根因是「地址各写各的」,那就把它们收拢到一个地方统一管。新建 `kernel/arch/x86_64/memory_layout.hpp`,把内核高半划成首尾相接的区段,每段 `(base, size)`,下一段的 base = 上一段 `base + size`:

```text
KMEM_BASE = 0xFFFF800000000000
  Heap      [0x...000000, 0x...100000)   1 MB
  MMIO      [0x...100000, 0x...140000)   256 KB (KMEM_MMIO_SIZE = 0x40000)
  Stack     [0x...140000, 0x...240000)   ↑ 每 task 4 页往上长
  DMA       [0x...240000, 0x...340000)   1 MB
  ext2 DMA  [0x...340000, 0x...440000)   1 MB
```

(区段大小取自源码常量;`KMEM_MMIO_SIZE = 0x40000`,故栈落在 `0x...140000`、位于 MMIO 之后,不再重叠。)

然后三个散落的魔法地址全部改成引用这里的常量:

```diff
- MMIO_VIRT_BASE     = 0xFFFF800000100000
+ MMIO_VIRT_BASE     = cinux::arch::KMEM_MMIO_BASE
- next_stack_vaddr   { 0xFFFF800000100000 }
+ next_stack_vaddr   { cinux::arch::KMEM_STACK_BASE }
- EXT2_DMA_VIRT_BASE = 0xFFFF800000400000
+ EXT2_DMA_VIRT_BASE = cinux::arch::KMEM_EXT2_DMA_BASE
```

以后加新区域(帧缓冲保留区、新 DMA 池……),只要在布局表插一行,后面 base 自动顺延。

## 防复发

这次排错留下三条条件反射。

**其一:MMIO 寄存器读出全零 / 值「无故变化」,第一反应是「页表映射被覆盖」,不是「设备坏了」。** 设备寄存器是映射在内存地址上的,读到的其实是「这个虚拟地址当前指向的物理页」。如果这个虚拟地址被另一段 `g_vmm.map()` 重新映射过(栈、DMA 缓冲、别的 MMIO),CPU 读到的就是别人写进去的内容。看到 `SSTS` 从 `0x113` 变 `0x0`,先怀疑地址被盖,再去怀疑硬件——顺序反了会浪费大量时间。

**其二:散落在各文件的硬编码虚拟地址,是定时炸弹。** 它们之间有没有重叠,没有任何单点代码能告诉你。对策就是这次做的——集中到一张 `memory_layout.hpp` 的 `(base, size)` 链式表,base 只在表头定义一次,所有模块引用符号常量。新加区域插一行,谁也不会再「偷偷挑了同一个地址」。

**其三:重排初始化顺序时,顺手审计「地址空间是否重叠」。** 重排本身不创造 bug,但它会把**潜伏的数据冲突**从「从没被在错误时间访问」变成「正好在错误时间被访问」。这次是栈 vs MMIO;换个场景可能是两段 DMA 缓冲、或堆和页表区域。所以每次动启动顺序、尤其把某段操作从「调度器启动前」挪到「调度器启动后」(或反过来),都要想一遍:这条路径新碰到的虚拟地址,跟谁可能撞?

---

## 参考

- AHCI ABAR / BAR5:HBA 寄存器块映射在内存地址上(MMIO),`MMIO_VIRT_BASE` 映射的就是 BAR5。「读 MMIO 寄存器得全零 = 页表映射被覆盖」的依据。OSDev AHCI:<https://wiki.osdev.org/AHCI>。
- x86-64 高半直接映射(`0xFFFF8000_00000000+` canonical 高半):内核虚拟布局落在该区间;AMD64 APM / Intel SDM Vol 1。
- 原始排查笔记:[001_init_thread_refactor_mmio_collision.md](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/document/notes/028e/001_init_thread_refactor_mmio_collision.md)。
