---
title: 053 · SMP 迁移竞态:一个抽函数的重构,踩出一个潜伏的 ctx 写花 bug
---

# 053 · SMP 迁移竞态:一个抽函数的重构,踩出一个潜伏的 ctx bug

> 052 让两个核真跑起了线程,当时留了一笔债:本机 QEMU `-smp 2` 下 AHCI identify 偶发踩时序竞态(单核完全正常)。这一章把那笔债还了——但还的方式有点戏剧性:它不是被直接查出来的,而是被一个**看起来人畜无害的重构**(把内联的启动逻辑抽成一个函数)踩成**每次必现**的 panic,才逼着把潜伏的竞态挖出来。根因是任务跨核迁移时,旧核存上下文、新核取同一份上下文,两头并发读写同一个 `ctx` 字段——写花之后 `ret` 跳到垃圾地址。修法对齐 Linux 的 `task_struct->on_cpu`:给每个任务一个「现在归哪个核」的标记,调度时跳过正在被别的核存上下文的任务。B 档:验证靠单核内核测试不回归(931 全绿)+ 双核 `-smp 2` panic 归零。
>
> 诚实边界:这套 `on_cpu` 纪律是「可调试优先于性能」的取舍——被跳过的任务留在队列里等下一轮,而不是死等;对 hobby OS 够用,生产内核会做得更精细(比如 Linux 的 wake/wait 机制)。本机 QEMU 的 `-smp 2` AHCI heisenbug 是环境相关的时序问题,这个修法堵的是它背后的**调度竞态根因**,不是 QEMU 本身的时序怪癖。

## 这章咱们要点亮什么

1. **潜伏竞态长什么样**:一个能跑的 SMP 调度器,为什么还会在「任务迁移」这个窗口写花上下文。
2. **heisenbug 怎么被坐实**:靠对照实验(内联 vs 抽函数、有内容 vs 空循环)+ 几行诊断打印,把「偶现」逼成「必现」,再定位到根因。
3. **`on_cpu` 纪律**:给任务标「归哪个核」,为什么这个标记能让存/取上下文不再撞车。
4. **跳过而不是死等**:为什么两个核互等对方存完会死锁,改成「挑别的任务」反而对。

## 一个抽函数的重构,踩出每次必现的 panic

故事从一个重构讲起。内核启动时有个二选一:GUI 模式启桌面、非 GUI 模式 fork 一个 shell。这两段一直是内联在 `kernel_init_thread` 里的(`#ifdef` 分叉)。有个重构把它们抽成一个共享的 `launch_userspace()` 接口——逻辑和内联版**完全等价**,单核跑测试 931 全过。

然后跑双核 `make run -smp 2`:**每次必 panic**。

有意思的地方来了:把重构 `git stash` 退回内联版,`-smp 2` 干干净净(panic count 0);换回抽函数版,每次 panic。更进一步的对照——把抽出来的 `gui_worker` 线程体改成空死循环(只 `yield`,啥也不干),`-smp 2` **照样 panic**。

这三组对照实验把范围钉死了:元凶不是 `gui_worker` 干了什么,而是**「多了一个任务 + 双核并发」本身**。抽函数只是让时序差了几个时钟,正好踩进一个一直潜伏着的竞态窗口。内联版不是没这个 bug,是时序恰好绕开了它——这是 heisenbug 最磨人的地方:它一直在,只是没被触发。

## 定位:同一份 ctx 被两头并发读写

加几行诊断打印,在 `schedule()` 的 `context_switch` 前打出 cpu/prev_tid/next_tid,铁证就出来了:

```
cpu1 tid3(kernel_init) -> tid5(gui_worker)   # gui_worker 在 cpu1 跑
cpu1 tid5 -> tid3                              # gui_worker 让出 cpu1,cpu1 存它的 ctx
cpu0 tid4 -> tid5                              # cpu0 同时切入 gui_worker,恢复同一份 ctx  ← 竞态窗口
```

问题清楚了:任务从 cpu1 迁移到 cpu0 时,**cpu1 的 `context_switch` 正在保存 `task->ctx`,cpu0 的 `context_switch` 已经在恢复同一份 `task->ctx`**。同一个 ctx 字段被并发一读一写,写花。写花之后 `ret` 跳到垃圾 RIP(实测 `0x1B` / `0xffffffff8002018e`——后者不在内核地址段,证实是 ctx 毁了之后跳到的随机地址),触发 #UD/#BP。

runqueue 是有锁的(`irq_guard`),所以**不是 runqueue 竞态**——坏的是 task 的 ctx 字段本身。052 的多核安全有个假设:「`pick_next` 把任务从队列摘走,就不会有两个核同时跑同一个任务」。这个假设挡住了「两核同时**运行**同一任务」,但**漏了迁移窗口**:旧核还在存、新核已经开始取。摘出队列和「上下文存完」之间有个时间差,窗口就在这儿。

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
