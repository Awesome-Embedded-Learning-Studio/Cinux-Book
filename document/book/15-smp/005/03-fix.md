---
title: 03 · 修法:给任务标「归哪个核」(`on_cpu`)与诚实边界
---

# 修法:给任务标「归哪个核」(`on_cpu`)与诚实边界

## 修法:给任务标一个「归哪个核」(`on_cpu`)

根子在于:调度器没法知道「这个任务的上下文是不是已经被存好了」。那就给它一个标记——对齐 Linux 的 `task_struct->on_cpu`:每个任务一个 `on_cpu` 字段,`-1` 表示「没在跑,上下文已存/可安全取」,`cpu_id` 表示「正在这个核上跑,上下文还在核里」。

```cpp
// F4-followup (SMP migration race): a fresh task has never run, so no CPU is
// saving its ctx.  on_cpu = -1 ("not running / ctx is saved"); schedule()
// sets a cpu_id before switching to it; pick_next() only picks on_cpu == -1.
task->on_cpu = -1;
```

（`task_builder.cpp`,`TaskBuilder::build()` 里给每个新任务初始化。)关键纪律是三个点配合:

**存完才放行。** `context_switch.S` 存完旧任务的 ctx 之后,**立刻**把它标记为「可取」。这一步必须在汇编里做,不能放到 C 层——因为 `context_switch` 的「返回」语义是「任务被切回来时」(在切回来的栈上),不是「prev 存完了」。所以「标记 prev 存完」只能在存完 from 的那几条指令之后立即写:

```asm
movl $-1, 96(%rdi)    # from->on_cpu = -1 (ctx save complete)
```

（`context_switch.S:78`。`%rdi` 是 Task 指针,ctx 在 Task+0,`on_cpu` 在 Task+`sizeof(CpuContext)`=96。)这是一条 x86 store,配合内存序保证别的核看得到。

**取之前先认领。** `schedule()` 决定切入一个任务前,先把它标记成「归我这个核」,用 release 序写:

```cpp
__atomic_store_n(&next->on_cpu, static_cast<int>(percpu()->cpu_id), __ATOMIC_RELEASE);
```

（`scheduler.cpp:467`,在 `schedule()` 主路径里。)认领之后才开始恢复它的 ctx。

**挑的时候跳过「正在被别核存的」。** `pick_next` 扫队列时,跳过那些 `on_cpu != -1` 且不是本核的任务——它们正被别的核存上下文,现在取会撞车:

```cpp
// F4-followup (SMP migration race): skip tasks whose on_cpu != -1 -- their
// ctx save is still in flight on the other CPU.
```

（`roundrobin.cpp:113`。)但有个细节:本核的任务**不跳过**。这是为了保住单核的语义——单核 `yield` 时 `next == prev`,如果不小心跳过了自己,就空转了。所以判据是「`on_cpu != -1 && on_cpu != 本核`」。

为了不让汇编去猜字段偏移,`process.hpp` 用 `static_assert` 把 `on_cpu` 钉死在 `sizeof(CpuContext)` 那个偏移上:

```cpp
static_assert(offsetof(Task, on_cpu) == sizeof(CpuContext), "on_cpu offset for context_switch.S");
```

（`process.hpp:326`。)布局一变编译期就炸,汇编里那条 `96(%rdi)` 才永远对得上。

## 为什么是「跳过」而不是「死等」

这里有个很容易想到、但会踩坑的方案:既然要等对方存完,那就让 `pick_next` 自旋等那个 `on_cpu` 变成 `-1` 呗?

不行,会死锁。想象两个核:cpu0 想取任务 A(正被 cpu1 存),cpu1 想取任务 B(正被 cpu0 存)。两边都自旋等对方存完——可对方要存完,就得推进自己的 `context_switch`,而它正卡在 `pick_next` 里等**你**存完。环形等待,死锁。

Cinux 的选择是 `pick_next` **跳过**正在被存的任务,挑队列里别的(都没有就 idle)。被跳过的任务留在队列里,等它的 `on_cpu` 变 `-1`(存完)之后,下一轮 `pick` 自然会选它。没人死等,系统一直推进。代价是可能多 idle 一下、调度没那么紧——对 hobby OS 来说,「可调试、不死锁」比「调度延迟最低」重要。生产内核(比如 Linux)会用更精细的 wake/wait 机制把等待的任务唤醒,但那是后续的事。

## 诚实边界

**这是「可调试优先」的取舍,不是最优解。** 被跳过的任务不会马上被唤醒,得等下一轮调度轮到。对教学内核够用;真要抠调度延迟,得加 wake 机制(存完 `on_cpu=-1` 后主动通知在等的核)。

**`on_cpu` 不进 lockdep 锁序图。** 它用的是 `__atomic` 原子存取,不是自旋锁,所以不参与死锁检测的锁序分析。`pick_next` 里跳过 `on_cpu` 的判断在 runqueue 既有锁的保护内,没新增锁。

**修的是调度竞态根因,不是 QEMU 时序怪癖。** 本机 `-smp 2` 的 AHCI heisenbug 表面是「QEMU 时序」,但根因是调度器的迁移窗口竞态。这个修法堵的是根因——竞态没了,不管 QEMU 时序怎么抖都不会再写花 ctx。

**单核不破坏。** `on_cpu` 纪律在单核下是 no-op:本核的任务不会被 `pick_next` 跳过,`yield` 时 `next==prev` 直接返回的语义保持不变。所以单核 `run-kernel-test` 仍然 931 全绿,双核 `-smp 2` panic 归零。

下一章回到 GUI 卷(054 GUI 解耦、055 xHCI USB)之后的下一条线;这套 SMP 加固到这里先收住。
