---
title: Lab 028e · init 线程化与一次潜伏碰撞的复盘
---

# Lab 028e · init 线程化与一次潜伏碰撞的复盘

> 028e 是「重构 + 排错」双主题,所以这个 lab 也分两半:前半让你把 init 线程模型和新内存布局**在纸上吃透**——画启动流程对照、手算区段地址、回答「如果改一个常量会撞到谁」;后半是排错演练——给你一个「MMIO 寄存器读出全零」的现象,让你独立走一遍假设→验证→定位的链路,把 028e 那次硬核排错变成可复用的方法。没有新代码要写,全是理解与推理。

## 实验目标

- 画出 028d「一根筋」与 028e「init 线程」两套启动流程的对照,能标出「调度器启动」这道分界线前后分别干了什么——并解释为什么这道分界线决定了 mmio 碰撞**发不发作**。
- 手算 `memory_layout.hpp` 五个区段的 `[base, end)`,验证栈不再与 MMIO 重叠。
- 能回答「改一个布局常量,会撞到谁」这类链式推理。
- 独立完成一次「MMIO 读全零」的排错演练,写出至少三条假设与各自的验证手段。
- 理解 `kernel_init_thread` 为什么用 `AHCI::instance()` 而不是直接引用全局变量(跨翻译单元链接)。

## 前置条件

- 028d 通过;理解 `Scheduler::run_first` / `schedule` / idle task、AHCI BAR5 是 MMIO、`VMM::map` 如何改页表。
- 028e 代码已构建:`cmake --build build`,`make run` 能起。
- 读懂主书第 028e 章的「Linux init 模型对齐」和「调试现场」两节,以及 [028e-mmio-stack-collision.md](../../debug-notes/028e-mmio-stack-collision.md)。

## 任务分解

### 任务 1:画两套启动流程的对照图

纸上画两张时序图,横向是时间,纵向是「谁在执行 / 在干什么」。第一张是 028d 的 `kernel_main`:

```text
kernel_main: ...初始化 → AHCI init → ext2.mount() → vfs_mount → run_concurrent_stress()
                                                          ↑
                                              （调度器在这里面才 init，建第一个内核栈）
```

第二张是 028e:

```text
kernel_main: ...初始化 → AHCI init → Scheduler::init() → spawn kernel_init / boot → run_first(boot)
                                          ↑
                                   （这里就建 idle 栈）
kernel_init 线程: ext2.mount() → vfs_mount → launch_first_user → exit_current
                          ↑
                  （此时内核栈已经映射过了）
```

在两张图上各标出**「第一次建立内核栈映射」**和**「第一次读 AHCI MMIO(挂载)」**的相对先后。然后回答:为什么 028d 的顺序里,即使两个魔法地址早就相等,挂载也不会炸?为什么 028e 一换顺序就炸?——这正是「重排顺序激活潜伏 bug」的核肉,要能用一句话讲清。

### 任务 2:手算内存布局,验证不重叠

打开 `kernel/arch/x86_64/memory_layout.hpp`,把五个区段的 `[base, end)` 填出来(`KMEM_BASE = 0xFFFF800000000000`):

```text
Heap      [0x...______, 0x...______)   size = ______
MMIO      [0x...______, 0x...______)   size = ______
Stack     [0x...______, 0x...______)   （上界由 DMA_BASE 决定）
DMA       [0x...______, 0x...______)   size = ______
ext2 DMA  [0x...______, 0x...______)   size = ______
```

填完检查:

- 栈区段的 base 是不是落在 MMIO 区段**之后**(即 `MMIO_BASE + MMIO_SIZE`)?这才是碰撞被修掉的关键。
- 栈的「上界」由谁决定?(`KMEM_DMA_BASE = KMEM_STACK_BASE + 0x100000`,所以栈可用 1 MB。)每个 task 4 页(16 KB),1 MB 能容纳多少个 task 的栈?超过会怎样?

> 注意:本 lab 的地址一律以**源码** `memory_layout.hpp` 的常量算出。原始排查 note 里写的栈基址 `0x...150000` 是早期草稿数字(与 `KMEM_MMIO_SIZE=0x40000` 不自洽),以源码为准——别照抄 note。

### 任务 3:链式推理——改一个常量,撞到谁?

不跑代码,纯推理:

- 如果把 `KMEM_MMIO_SIZE` 从 `0x40000` 改成 `0x10000`(64 KB),栈的 base 会变成多少?它会不会和谁重叠?(提示:栈 base = `MMIO_BASE + 新 MMIO_SIZE`,但 MMIO 区段实际占多大由谁说了算?)
- 如果未来要加一个「帧缓冲保留区」,插在 DMA 和 ext2 DMA **之间**,`KMEM_EXT2_DMA_BASE` 会自动顺延吗?为什么?(看布局表是 `base + size` 链式的含义。)
- 如果有人图省事,在某个新驱动里又写了个 `static constexpr MMIO = 0xFFFF800000100000`,会发生什么?——这正是 028e 修复要杜绝的事。

### 任务 4:读 init.cpp,理解跨翻译单元的实例访问

`kernel_init_thread` 在 `init.cpp` 里(`big_kernel_common` 库),那个 `static AHCI ahci` 实例却在 `main.cpp` 里。回答:

- 如果 `init.cpp` 直接引用一个 `main.cpp` 的全局 `AHCI` 变量,**测试内核**(`big_kernel_test`,没有 `main.cpp` 的那个定义)会怎样?(链接期还是运行期出问题?)
- `AHCI::instance()` / `set_instance()` 这对静态访问器是怎么绕开这个问题的?它把「实例在哪」的状态放在类的哪里?
- `main.cpp` 在什么时机调用 `set_instance`?`init.cpp` 在什么时机读 `instance()`?如果顺序反了(先 spawn init 线程、再 set_instance)会怎样?

### 任务 5(排错演练):MMIO 读出全零,怎么办?

假设你在做一个**新的**重构,现象是:某设备的一段 MMIO 寄存器,初始化时读到正常值(比如 `0x113`),进入某个新线程后读到 `0x0`。设备「读着读着消失了」。请你独立写出**至少三条**排查假设,每条配一个**可执行的验证手段**(改什么、加什么打印、跑什么),并预判各自的结果会指向什么。

参考方向(自己先写再看):

- 假设 A:页表映射被覆盖(有别的 `g_vmm.map()` 把这段虚拟地址重新映射了)——验证:打印可疑路径上所有 `map()` 的目标虚拟地址,看有没有落进这段 MMIO 区间;或在该寄存器读取前后打印 `VMM::translate(那个虚拟地址)` 看物理地址有没有变。
- 假设 B:被抢占/中断打断——验证:在该路径外包 `InterruptGuard` 关中断复现,问题消失则成立(028e 正是这样**证伪**了这条)。
- 假设 C:执行顺序变了、这段读取发生在「映射还没建好/已被改」的时刻——验证:对比重构前后的时序,标出「第一次 map 这段地址」与「第一次读它」的先后。

把这套假设→验证→定位的链路写成一段复盘,核心是:**先证伪最直觉的那个假设(028e 是「抢占」),别一路错下去。**

## 接口约束

- `cinux::proc::kernel_init_thread()`:`init.cpp`,接管 ext2 mount + VFS + `launch_first_user` + `exit_current`。
- `cinux::drivers::ahci::AHCI::instance()` / `set_instance(AHCI*)`:跨翻译单元访问 AHCI 单例。
- `cinux::arch::KMEM_{HEAP,MMIO,STACK,DMA,EXT2_DMA}_{BASE,SIZE}`:`memory_layout.hpp` 的统一布局常量,base+size 链式。
- `Scheduler::init()` / `run_first(Task*)` / `add_task(Task*)`:init 模型的发动点。
- `Scheduler::current()` / `set_current(Task*)`:init 线程里获取/设置当前 task。

## 验证步骤

- **任务 1–4**:纸上完成;任务 2 的地址表可对照 `memory_layout.hpp` 源码自检。
- **任务 5**:写完假设链路后,对照主书「调试现场」和 [028e-mmio-stack-collision.md](../../debug-notes/028e-mmio-stack-collision.md),看你的假设是否覆盖了「页表覆盖」这条真根因、以及是否正确地**先证伪了「抢占」这条红鲱鱼**。
- **端到端**:`make run` 启动生产内核,串口应看到 `[INIT] kernel_init started` → ext2 挂载**成功**(无 command timeout)→ `[VFS] ext2 mounted at /` → shell。
- **回归点**:在 init 线程里读 Port 1 的 `SSTS`,应保持 `0x113`(不再是 `0x0`)——直接证明「栈盖 MMIO」没了。

## 常见故障

- **手算时栈 base 算成 `0x...100000`**:忘了栈 base = `MMIO_BASE + MMIO_SIZE`。`MMIO_SIZE = 0x40000`,所以是 `0x...140000`。算错这一步,后面「是否重叠」的结论就全错了。
- **照抄 note 的 `0x...150000`**:note 那个数字与源码 `KMEM_MMIO_SIZE` 不自洽,是草稿残留。以 `memory_layout.hpp` 源码为准。
- **以为「重排顺序创造了 bug」**:不是。冲突早就成立(两个地址一直相等),重排只是让它第一次在错误的时间被访问。措辞要准:「激活潜伏冲突」,不是「引入新 bug」。
- **把 Linux init 的完整能力安到 Cinux 头上**:Cinux 只借了「boot 当 handoff 源 + init 线程接管」这个**模型**,没有 fork、进程树、信号、wait。别说成「实现了 init 子系统」。
- **又把栈基址写回 `0x...100000`**:`make run` 会再现 command timeout、`SSTS=0x0`——这就是碰撞复发的信号,回去查布局表。

## 通过标准

- 任务 1 能在两张时序图上正确标出「第一次建栈」与「第一次读 MMIO」的先后,并用一句话解释「为什么换顺序就发作」。
- 任务 2 五个区段地址全对(以源码为准),栈 base 落在 MMIO 之后。
- 任务 3 能推出「改 MMIO_SIZE 影响栈 base」并解释链式 base+size 的好处。
- 任务 4 能说清跨翻译单元链接问题和 `instance/set_instance` 的解法。
- 任务 5 写出至少三条假设(含「页表覆盖」真因 + 「抢占」红鲱鱼),且明确「先证伪红鲱鱼」。
- `make run` 跑通,`SSTS` 保持 `0x113`。
- 能口头回答:为什么散落的硬编码虚拟地址是炸弹?MMIO 读全零第一反应该怀疑什么?
