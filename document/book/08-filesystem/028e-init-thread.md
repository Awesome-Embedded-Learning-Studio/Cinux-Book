---
title: 028e · init 线程:让启动流程能被调度、能阻塞
---

# 028e · init 线程:让启动流程能被调度、能阻塞

> 028d 的结尾我们留了一个尴尬:`kernel_main` 从头跑到尾——AHCI 读盘、ext2 挂载、起 shell——全是同步的,跑在一个不可调度的上下文里。哪天哪一步想「等一会」(等磁盘就绪、等一个事件),没有干净的地方让它睡,因为启动代码压根不在任何可调度的线程上;028d 辛辛苦苦造好的 `Mutex`/`Semaphore` 也就一直没处用。这一章来兑现:把启动流程本身重构成 **init 线程**——对齐 Linux 那套「boot 上下文 → idle、PID 1 的 init 线程接管一切」的模型。
>
> 但这次重构不是「照着模型改完就收工」。改完一跑,ext2 挂载直接失败。于是这一章大半篇幅是一次硬核排错:顺着一个「设备读着读着消失了」的现象,一层层剥到一个潜伏了好几个 tag 的虚拟地址碰撞。换句话说,这一章给你两样东西——一个线程化的启动模型,和一次「重排执行顺序如何激活潜伏 bug」的真实复盘。

## 为什么现在需要它:启动逻辑不在任何可调度的线程上

先看清 028d 的启动路径长什么样。`kernel_main` 里大致是这个顺序:

```text
... GDT/IDT/PIC/PIT/PMM/VMM/Heap/帧缓冲/键盘 ...   ← 一堆初始化
AHCI init + 读扇区 0
ext2.mount()                  ← 挂载
vfs_mount_init() + mount "/"
run_concurrent_stress()       ← 进调度器、起 stress 线程、最后 launch_first_user
键盘轮询 while(1){ hlt; poll }   ← 兜底循环
```

问题在于:`ext2.mount()`、`vfs_mount_add`、`launch_first_user` 这些都发生在**调度器启动之前**,或者发生在 `run_concurrent_stress` 内部那个临时搭的 stress 框架里。它们都不属于任何「正经的可调度线程」。后果是:

- 启动中途想做 `Scheduler::yield()` / `block()`?不行,你不在运行队列里,调度器不认识你。
- 028d 的 `Mutex`/`Semaphore` 想用在启动路径(比如等磁盘)?没处下嘴。
- 那个临时的 `run_concurrent_stress` 框架,本来是 028d 用来验证并发安全的脚手架,现在却**兼着**「启动到 shell」的职责——两件事纠缠在一个函数里,不干净。

028e 要做的就是解耦:调度器在 `kernel_main` 里直接初始化,然后 spawn 一个 `kernel_init` 线程,让**它**去挂文件系统、起 shell。这样一来,启动逻辑就跟普通内核线程一样,能被调度、能阻塞、能让出——028d 的阻塞原语终于有了用武之地。

## Linux init 模型对齐:boot、init、idle 三个角色

Cinux 借的是 Linux 的**模型**(注意,只是模型,不是 Linux init 的全部能力),三个角色:

```text
   kernel_main (boot CPU 的原始执行流)
        │
        │  Scheduler::init()        ← 建好调度器 + idle task
        │  spawn kernel_init 线程    ← PID 1 的类比:接管 FS + shell
        │  spawn boot task          ← boot CPU 上下文成为这个 task
        │  run_first(boot_task)     ← boot 成为 current，随后切给 kernel_init
        ▼
   ┌─────────────────────────────────────────────┐
   │  kernel_init 线程 (PID 1 类比)                 │
   │    AHCI::instance() → ext2.mount()            │
   │    vfs_mount_init() + mount "/"               │
   │    launch_first_user()  ← shell               │
   │    exit_current()                             │
   └─────────────────────────────────────────────┘
        │ shell 退出 / 无事可做时
        ▼
   idle task (Scheduler::init 创建):hlt 空转
```

- **idle task**:调度器自己的,`Scheduler::init()` 里就建好了,运行队列空时由它 `hlt` 空转(028d 已有)。
- **boot task**:`run_first(boot_task)` 把 boot CPU 当前的执行流「认领」成一个 task,作为 handoff 的起点。它的 entry 是个只打印 `UNEXPECTED` 然后 `hlt` 的 lambda——正常情况下控制权一交出去就再也不回来,真回到它说明哪里错了。
- **kernel_init**:`init.cpp` 里的 `kernel_init_thread`,干 PID 1 的活:挂 ext2、挂 VFS、`launch_first_user` 起 shell,最后 `exit_current`。

`main.cpp` 的步骤 22 就是这一切的发动点:

```cpp
Scheduler::init();

auto* init_task = TaskBuilder()
    .set_entry(cinux::proc::kernel_init_thread)
    .set_name("kernel_init")
    .build();
Scheduler::add_task(init_task);

auto* boot_task = TaskBuilder()
    .set_entry([]() { /* UNEXPECTED, hlt */ })
    .set_name("boot")
    .build();
Scheduler::run_first(boot_task);   // boot 成为 current，随即调度到 kernel_init
```

而 `kernel_init_thread` 本体非常薄,就是把原来散在 `kernel_main` 末尾的「挂载 + 起 shell」搬进来:

```cpp
void kernel_init_thread() {
    auto* self = Scheduler::current();
    kprintf("[INIT] kernel_init started tid=%u\n", self ? self->tid : 0);

    static Ext2 ext2(AHCI::instance(), 1);
    if (!ext2.mount()) { kprintf("[INIT] ext2 mount failed!\n"); }

    vfs_mount_init();
    vfs_mount_add("/", &ext2);

    launch_first_user();          // shell
    Scheduler::exit_current();
}
```

> ⚠️ 注意措辞:Cinux 借的是「init 线程」这个**组织模型**(boot 当 handoff 源、一个 init 线程接管后续),**没有** Linux 的 fork、进程树、信号、wait 那一整套。别把它读成「Cinux 实现了 init 子系统」。

## 三个配套小修

线程化不是孤立的一刀,它逼出了三处连带修改。

### 一、AHCI 实例怎么从 init.cpp 拿到:instance()/set_instance()

`kernel_init_thread` 在 `init.cpp` 里,而 `init.cpp` 属于 `big_kernel_common`(被生产内核和测试内核共用);那个 `static AHCI ahci` 实例却定义在 `main.cpp` 里。如果 init 线程直接引用一个 `main.cpp` 的全局变量,测试内核(没有那个定义)会链接失败。

解法是给 `AHCI` 加一对静态访问器,把「实例在哪」这件事封装进类自己:

```cpp
// ahci.hpp
static AHCI& instance();
static void  set_instance(AHCI* ahci);
// ahci.cpp
AHCI* AHCI::s_instance_ = nullptr;
AHCI& AHCI::instance()       { return *s_instance_; }
void  AHCI::set_instance(AHCI* a) { s_instance_ = a; }
```

`main.cpp` 初始化完 ahci 就 `AHCI::set_instance(&ahci)`,`init.cpp` 用 `AHCI::instance()` 拿。比暴露一个全局变量干净,也顺带修了跨翻译单元的链接问题。

### 二、launch_first_user 不再手搓 Task:复用调度器的 current

028d 的 `launch_first_user` 里有一行很可疑的代码:它手动 `static Task shell_task{}`——一个零初始化的、没有内核栈、`tid=0`、不在任何运行队列里的「假 Task」。重构前凑合能用,是因为那时调度器还没真正接管它;但一旦启动路径线程化,这个假 Task 就成了定时炸弹:

- `sys_exit` 会 `Scheduler::current()` 拿到这个假 Task,标记 Dead 后 `yield`;可它根本不在运行队列里;
- 它没有有效的 `CpuContext`,调度器要是尝试切回它就直接崩。

修法是**别再造 Task**:`launch_first_user` 现在跑在 `kernel_init` 线程里,直接用 `Scheduler::current()` 拿到这个真 Task,只更新它的 `addr_space` 和 `cwd`:

```cpp
auto* current = Scheduler::current();
current->addr_space = user_space;
current->cwd[0] = '/';
current->cwd[1] = '\0';
Scheduler::set_current(current);
```

还有一个连带的小坑,分两层。028d 里 `user_space` 是 `launch_first_user` 的一个**栈上局部变量**(`AddressSpace user_space;`),函数一返回就析构;但线程化之后,它要被存进 `current->addr_space`、由调度器长期持有,函数返回时不能析构,所以必须把它的生命期提升到**静态存储**。可 `AddressSpace` 带析构器,一个静态的 `AddressSpace` 对象会触发 `__dso_handle` 之类的全局析构登记,在 freestanding 内核里链接报错。解法是 **placement new**——在一块 align 好的静态 `uint8_t` buffer 上构造它,既拿到静态生命期,又绕开析构器登记:

```cpp
alignas(alignof(AddressSpace)) static uint8_t user_space_storage[sizeof(AddressSpace)];
auto* user_space = new (user_space_storage) AddressSpace;   // 不触发全局析构器
```

### 三、main.cpp 删掉 stress 调用和键盘轮询

`run_concurrent_stress()` 是 028d 的阶段性脚手架,兼着「验证并发 + 顺带起 shell」两件事。028e 把启动职责交还给 `kernel_init` 后,这个脚手架就整个删掉了(`kernel/stress/stress_test.cpp` 连文件一起移除)。`kernel_main` 末尾也不再是键盘轮询循环,而是让 `boot_task` 进入调度器、由 idle task 兜底。

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
AHCI MMIO 虚拟基址 (ahci.cpp, 028d):   0xFFFF800000100000
内核栈虚拟起始     (process.cpp, 028d): 0xFFFF800000100000   ← 完全相同
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

因为**执行顺序**。028d 时,`Scheduler::init()`(也就是第一次映射内核栈的那一刻)发生在 `run_concurrent_stress()` 里;而 `ext2.mount()` 发生在 `kernel_main` 里、`run_concurrent_stress()` **之前**。也就是说:**挂载完成时,调度器还没启动,还没有任何内核栈被映射,MMIO 区域完好无损**。挂载一结束,后面才轮到调度器建栈——那时栈盖掉 MMIO 也无所谓了,因为已经没人再读它。

028e 的重构把顺序换了:`Scheduler::init()` 提到 `kernel_main` 里、`ext2.mount()` 挪进 `kernel_init` 线程(在调度器启动**之后**)。于是「建栈」发生在「挂载」之前,潜伏的碰撞被**激活**了。

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

## 验证

028e 没有新增单测(它是一次结构性重构,验证靠的是「真的启动起来、跑通整条链路」)。

```bash
cmake --build build
make run          # 启动生产内核（挂 AHCI 盘 + ext2 盘）
```

串口里应当看到新的 init 线程路径,而且 ext2 挂载**成功**(碰撞已修):

```text
[BIG] ===== Scheduler & Init Thread =====
[INIT] kernel_init started tid=...
[INIT] ===== Milestone 028: ext2 Filesystem =====
[AHCI] ...                                          ← 不再有 command timeout
[INIT] ===== Milestone 027: VFS =====
[VFS] ext2 mounted at /
[INIT] ===== Milestone 023: Syscall from Ring 3 =====
... shell 起来
```

**关键的回归验证点**:在 init 线程里 Port 1 的 `SSTS` 应当**保持 `0x113`**,不再是 `0x0`——这直接证明「栈盖 MMIO」的碰撞没了。如果它又掉成 `0x0`,说明你又把栈基址写回了 `0x...100000`,或者布局表里区段算错了。

测试内核照常可跑(`make run-kernel-test`),它不含这次重构的 init 路径,但 028d 那套 RAII 同步测试还在。

> 顺带:028d 那个临时搭的 `run_concurrent_stress()` 框架(`kernel/stress/stress_test.cpp`)在 028e 整个删掉了——它兼着的「起 shell」职责已交还给 `kernel_init`。别把它和 `cmake/qemu.cmake` 里那个 `run-stress-test` 目标搞混:后者是用 1 GB 合成 ELF 压测 mini-loader 装载大内核的另一条测试线,跟这次重构无关、028e 里照常可用。至于原 stress 框架承担的并发验证,现在由生产启动 `make run` 本身覆盖。

## 下一站

028e 让启动路径成了可调度的 init 线程,顺手把散落的内核虚拟地址收拢成一张布局表。到此,内核这边「能调度、能阻塞、地址不打架」都齐了,帧缓冲也早就点亮了(013)。下一章(029)要做的,是**在帧缓冲上画东西**——从「能往屏幕打字」走向「能在屏幕上画图形」。有了能阻塞、能让出的 init 线程,绘图循环、刷新时序这些也才有了干净的落点。具体怎么画,那是 029 的事。

---

**参考**

- Linux 内核的 idle/init 线程模型(PID 0 idle、PID 1 init 接管用户态启动):组织模型来自内核源码 `init/main.c` 的 `rest_init()`——它派生 `kernel_init`(PID 1)与 idle(PID 0),前者接管后续启动、后者进空闲循环。本章只借这个**模型**(boot 当 handoff 源 + init 线程接管),并非实现 init 全套能力。<https://github.com/torvalds/linux/blob/master/init/main.c>
- AHCI 的 ABAR / BAR5(HBA 寄存器块的 MMIO 基址):`MMIO_VIRT_BASE` 映射的就是 AHCI 的 BAR5;「读 MMIO 寄存器得全零 = 页表映射被覆盖」的依据。OSDev AHCI:<https://wiki.osdev.org/AHCI>
- placement new 的语法语义:`launch_first_user` 在对齐的静态 buffer 上构造 `AddressSpace` 所用。<https://en.cppreference.com/w/cpp/language/new>。至于「静态对象因带析构器会触发 `__dso_handle`/`__cxa_atexit` 登记、在 freestanding 下链接报错」这一层,属内核 freestanding 链接的常见经验(Itanium C++ ABI 的析构器登记机制),并非来自该页。
- x86-64 高半直接映射(`0xFFFF8000_00000000+` 的 canonical 高半区):内核虚拟内存布局落在该区间;区段化的 `memory_layout.hpp` 的地址依据。AMD64 APM / Intel SDM Vol 1(虚拟地址与 canonical 地址)。
