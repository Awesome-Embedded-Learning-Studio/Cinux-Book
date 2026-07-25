---
title: 088 · timer_queue 与 stats_kthread:把定时唤醒和周期采样钉进内核
---

# 088 · timer_queue 与 stats_kthread:把定时唤醒和周期采样钉进内核

> 084 章你用 poll 有限 timeout 真睡了一个任务——`prepare_to_wait` 翻 Blocked、`timer_queue_arm` 挂一个 30ms 的 deadline、`schedule_blocked` 切走,30ms 后 timer tick 把它叫醒返回 0。那套用法的骨架是 084 章讲的;但 `timer_queue_arm` 内部到底挂了什么、谁负责到点叫醒、为什么不会和运行队列撞锁——那半边没讲。这一章就是把那半边翻给你看。
>
> 同一章还有第二件基础设施:`stats_kthread`,一个常驻内核线程,每秒把 PMM 余量 / slab 页 / PageCache 占用 / 累计缺页数打一次串口,画成一条曲线给「g++ hello.cpp 卡顿」这类 workload 做性能剖析。这玩意儿本身代码很短(95 行),真正值钱的是它的调度难点——它必须坐在 band 0(和 init、cc1 同一优先级档),既不能太低(会被编译饿死、采不到样)、也不能太高(会抢占 user code 扭曲测量数),还不能 `sti/hlt`(会和 071 章那个 sti/hlt 坑同族,把 gate 卡死在第一次 fork)。围绕这三个角的不可能三角——两版实测失败(priority 250 饿死、band 0 + sti/hlt 卡死)+ 一版逻辑排除(priority 提高会扭曲测量)——最后推出来的正解是 `yield`,一个同时承担「让 CPU 给被测对象」和「等下一个 tick」两件事的姿势。这一章把这条推理链完整走一遍。
>
> 章末是 §14 file gate:CMake 用一个 `CINUX_STATS_KTHREAD` option 选链 `stats_kthread.cpp`(真实现)还是 `stats_kthread_stub.cpp`(空函数),源码零 `#ifdef`,`init.cpp` 无条件调 `start_stats_thread()`。这是 Cinux 把可选增强收进编译选项的标准模式(usb_stub / tlb_drain_stub 同款)。

## 这章咱们要点亮什么

1. **timer_queue 的内部**:一个容量 32 的固定表 `Entry{Task*; uint64_t deadline_ns; bool armed}`([timer_queue.cpp:34](../../../kernel/proc/timer_queue.cpp#L34)),`Spinlock` 守护。三接口——`timer_queue_arm`(`:54`)关 IRQ 拿锁、先扫一遍替换已有 arm、再找空槽、表满返 `false` 让调用方降级成无 timeout park 绝不挂死;`timer_queue_disarm`(`:77`)醒来清自己的槽防 stale wakeup;`timer_queue_tick`(`:90`)由 PIT 驱动的 `Scheduler::tick`(`scheduler.cpp:367` 定义)每次中断在 `:379` 调,锁内收集过期 task 到栈上 `fired[]`、释放锁后才 `unblock`。这条「锁外唤醒」是 timer_queue 唯一真正值得学的并发点。
2. **为什么是固定表不是堆/红黑树**:头注释([timer_queue.cpp:5-9](../../../kernel/proc/timer_queue.cpp#L5))写明「a linear scan of kMaxTimers is cheap at hobby scale... and keeps the structure trivially correct」——32 槽远超实际并发定时等待数,O(n) 在 PIT tick 频率下成本可忽略,换来结构平凡正确(无堆下滤、无树旋转)。这是「结构平凡地正确」的支点。
3. **stats_kthread 是干什么的**:常驻 kthread,每 ~1 秒(1e9 ns)调一次 `dump_memory_stats()`([diagnostics.cpp:26](../../../kernel/mm/diagnostics.cpp#L26)),把 PMM/slab/PageCache/#PF/I/O 五行打到串口形成曲线。计时基准是 HPET free-running counter(`stats_kthread.cpp:46-51`),HPET 不可用时退化成数 tick(`++ticks_since_dump >= 100`,`:67-72`)。
4. **band 0 kthread 不能 sti/hlt——本章明星**:stats kthread 必须 band 0(低会被饿死、高会扭曲测量),但 band 0 内「等 1 秒」是个大坑。三版尝试的调试叙事(症状→根因→定位→修复→防复发):(1) priority 250 + yield → CPU 密集下被永久饿死,0 行 dump(实测);(2) priority 提高 → 抢占 user code 扭曲测量(逻辑排除,没真跑);(3) band 0 但 sti/hlt → tick 唤醒后 quantum 没耗尽继续选自己,init/child 永远 Ready,gate 卡在第一次 fork、#PF 全程 +0(实测)。正解是 band 0 + yield,与 071 章 sti/hlt 坑同族(都是 Cinux 里 sti/hlt 用错上下文,但机制不同,后文详述)。
5. **§14 file gate 的粒度**:CMake option `CINUX_STATS_KTHREAD`(默认 OFF,[options.cmake:44](../../../cmake/options.cmake#L44))。ON 链 `stats_kthread.cpp`、OFF 链 `stats_kthread_stub.cpp`([CMakeLists.txt:18-24](../../../kernel/mm/CMakeLists.txt#L18))。关键设计——把 gate 放在「整个周期线程」这一层,不是每行 `#ifdef`;PF 计数器和 dump 的 #PF 行是无条件编的(panic 时也能看 #PF),只有「周期采样」这个 ad-hoc profiling 增强被 gate。
6. **dump_memory_stats 的四条正交维度 + PF delta**:`dump_memory_stats` 把「内存压力」拆成 PMM 余量 / slab 页 / PageCache 占用 / 累计 #PF 四条每次同步报全,后来追加第 5 条 ext2 I/O。关键设计是 #PF 用 `static uint64_t last_pf`([diagnostics.cpp:53](../../../kernel/mm/diagnostics.cpp#L53))跨调用算 delta——profiling 靠趋势不靠单点。
7. **诚实的验证边界**:timer_queue 没有独立单测,靠 084 的 `test_poll_finite_timeout_parks_then_returns_zero`([test_poll.cpp:191-204](../../../kernel/test/test_poll.cpp#L191))间接证 arm→park→tick 唤醒→disarm 整链;stats_kthread 没有单测,靠 ON + make run + 串口曲线(CinuxOS dev note 的 31s g++ 曲线是实测,Book 本地未重跑;注意 dev note 那条「126 行 dump」是 ext2 I/O 行还没加进 dump 之前的 4 行/dump 计数,当前源码每次 dump 6 行 [MEM],31s 复现应是 ~186 行);`test_memory_stats.cpp` 测的是 `dump_memory_stats` 这个 FO 组件不是 kthread 本身。这两条边界都要如实标,不拔高。

## timer_queue 的内部:固定表 + tick 扫过期(承接 084 的用法)

084 章你用过 `timer_queue_arm`/`disarm` 那两个接口——poll 有限 timeout 路径在 `InterruptGuard` 窗口里 `prepare_to_wait` + `timer_queue_arm` + `schedule_blocked`,醒来后 `timer_queue_disarm`。那是**用法**。这节讲**内部**。

先说动机。Cinux 调度器本来就能 park 一个 Blocked 任务——`prepare_to_wait` 翻 state、`schedule_blocked` 切走、`Scheduler::unblock` 把它翻 Ready。这套 proven 模板是 071 章立的,084 章一字不动地复用。但模板只有「睡」和「叫」两半,缺「**到点自动叫**」这半边。`sys_nanosleep` 至今没补这半边——它仍是 yield 轮询 HPET 烧 CPU:

```cpp
// kernel/syscall/sys_nanosleep.cpp:50-55(本章如实标它没迁 timer_queue)
// Poll the monotonic counter until the deadline, yielding between checks.
// NOT sti/hlt (the #DF hazard), NOT an IRQ wake (the F5-M4 follow-up); the
// yield gives other tasks the CPU so this is not a hard spin.
while (monotonic_ns() < deadline) {
    cinux::proc::Scheduler::yield();
}
```

`nanosleep` 头注释([timer_queue.hpp:5-10](../../../kernel/proc/timer_queue.hpp#L5),节选)提了 nanosleep 是 timer_queue 的目标用户,但 `.cpp` 里 `sys_nanosleep` 还在 yield 自旋——**以 `.cpp` 为准**,这条现在其实没生效。真正吃 timer_queue 的是 084 的有限 timeout poll。timer_queue 补的是「到点自动唤醒」这缺失的半边。

数据结构很简单,一个容量 32 的固定表:

```cpp
// kernel/proc/timer_queue.cpp:34-43
constexpr uint32_t kMaxTimers = 32;

struct Entry {
    Task*    task;
    uint64_t deadline_ns;
    bool     armed;
};

Entry    g_entries[kMaxTimers];
Spinlock g_lock;
```

一个 `Entry` 就三字段:被等的 task、绝对 deadline(纳秒)、是否 armed。整张表是全局单例 `g_entries[32]`,一把 `Spinlock` 守护。`kMaxTimers = 32` 的注释([timer_queue.cpp:31-33](../../../kernel/proc/timer_queue.cpp#L31))说清楚——「Plenty for a hobby OS (poll/select/nanosleep across a few tasks); if exhausted, arm() returns false and the caller falls back to sleeping on its wait queue without a timeout (never hangs)」。32 槽对一个 hobby OS 远远够用,真满了 `arm` 返 `false`,调用方降级成无 timeout park,**绝不挂死**。

三个接口逐个看。`timer_queue_arm`([timer_queue.cpp:54](../../../kernel/proc/timer_queue.cpp#L54)):

```cpp
// kernel/proc/timer_queue.cpp:54-75
bool timer_queue_arm(Task* task, uint64_t deadline_ns) {
    if (task == nullptr) {
        return false;
    }
    auto g = g_lock.irq_guard();
    // Replace an existing arm for this task (a re-park after a spurious wake).
    for (uint32_t i = 0; i < kMaxTimers; ++i) {
        if (g_entries[i].armed && g_entries[i].task == task) {
            g_entries[i].deadline_ns = deadline_ns;
            return true;
        }
    }
    for (uint32_t i = 0; i < kMaxTimers; ++i) {
        if (!g_entries[i].armed) {
            g_entries[i].task        = task;
            g_entries[i].deadline_ns = deadline_ns;
            g_entries[i].armed       = true;
            return true;
        }
    }
    return false;  // table full
}
```

两段循环。第一段扫一遍找有没有同一个 task 的已有 arm——有就替换 deadline。这支持「假唤醒后重 park」:你被 fd 叫醒重扫发现没就绪,回 for 顶再 arm 一次,新 deadline 覆盖旧的,不会留下 stale 槽位。第二段找空槽挂上。表满返 `false`——084 poll 那边接到 `false` 就当无 timeout 处理(`armed=false`,`will_sleep` 仍可由 `registered` 撑),绝不在 arm 这步挂死。整段在 `g_lock.irq_guard()` 下——关 IRQ 拿锁,因为 `arm` 必须和 `tick` 互斥(`tick` 也在 IRQ 上下文跑)。

`timer_queue_disarm`([timer_queue.cpp:77](../../../kernel/proc/timer_queue.cpp#L77)):

```cpp
// kernel/proc/timer_queue.cpp:77-88
void timer_queue_disarm(Task* task) {
    if (task == nullptr) {
        return;
    }
    auto g = g_lock.irq_guard();
    for (uint32_t i = 0; i < kMaxTimers; ++i) {
        if (g_entries[i].armed && g_entries[i].task == task) {
            g_entries[i].armed = false;
            g_entries[i].task  = nullptr;
        }
    }
}
```

醒来后清掉自己的槽位——不管是 timer 叫醒的还是 fd 叫醒的。不清的话,下一轮 tick 还会拿这个过期 deadline 把一个已经 Running 的 task 再 `unblock` 一次(虽然 `unblock` 幂等,见 [scheduler_block.cpp:48-59](../../../kernel/proc/scheduler_block.cpp#L48),但留着 stale 槽位本身是脏的,迟早撞上别的 task 重用这个 `Task*`)。`disarm` 遍历全表清所有匹配 task 的槽——理论上一个 task 不会同时在两个槽,但 defensive 写法。

`timer_queue_tick`([timer_queue.cpp:90](../../../kernel/proc/timer_queue.cpp#L90))是核心:

```cpp
// kernel/proc/timer_queue.cpp:90-112
void timer_queue_tick() {
    uint64_t now = monotonic_ns();

    Task*    fired[kMaxTimers];
    uint32_t nfired = 0;
    {
        auto g = g_lock.irq_guard();
        for (uint32_t i = 0; i < kMaxTimers; ++i) {
            if (g_entries[i].armed && g_entries[i].deadline_ns <= now) {
                fired[nfired++]    = g_entries[i].task;
                g_entries[i].armed = false;
                g_entries[i].task  = nullptr;
            }
        }
    }

    // Wake outside the timer lock (unblock takes the run-queue lock + may IPI).
    for (uint32_t i = 0; i < nfired; ++i) {
        if (fired[i] != nullptr) {
            Scheduler::unblock(fired[i]);
        }
    }
}
```

它由 PIT 驱动的 `Scheduler::tick`([scheduler.cpp:367](../../../kernel/proc/scheduler.cpp#L367))每次中断调:

```cpp
// kernel/proc/scheduler.cpp:367-379(节选,后续 task_tick 省略)
void Scheduler::tick() {
    Task* cur = current();
    if (!initialized_ || cur == nullptr) {
        return;
    }

    tick_count_.fetch_add(1, lib::MemoryOrder::Relaxed);

    // F8-M5 timer-wake: unblock any parked poll/select/nanosleep whose deadline
    // passed since the last tick.  Cheap no-op when nothing is armed (one locked
    // scan of a small table); runs before preemption so a woken task is runnable
    // for the pick below.
    timer_queue_tick();
    // ... 后续 quantum 记账、task_tick ...
}
```

`timer_queue_tick` 跑在 `Scheduler::tick` 最前——先唤醒过期 task,让它们在紧接着的 pick 里可被选中。注意 tick 的两段:锁内只做「收集 + 清槽位」(把过期 task 的指针存进栈上 `fired[]`、槽位置 `armed=false`);锁释放后才循环 `unblock`。

### 锁外唤醒的锁序纪律——本章第一个真值得学的并发点

为什么不在持有 timer spinlock 时直接调 `unblock`?看 `Scheduler::unblock` 干什么([scheduler_block.cpp:48](../../../kernel/proc/scheduler_block.cpp#L48)):

```cpp
// kernel/proc/scheduler_block.cpp:48-70(节选)
void Scheduler::unblock(lib::NotNull<Task*> task) {
    if (task == nullptr) {
        return;
    }

    // Idempotent (F4-M4 prepare-to-wait): only a still-Blocked task needs waking.
    if (task->state != TaskState::Blocked) {
        return;
    }

    task->state = TaskState::Ready;
    if (task->sched_class == nullptr) {
        task->sched_class = &default_rr_;
    }
    task->sched_class->enqueue(task);   // <- 拿 run-queue 锁
    // ... arch::wake_idle_ap() <- 可能发 IPI ...
}
```

`unblock` 内部要拿 run-queue 锁(`enqueue` 进运行队列)、可能发 IPI(`arch::wake_idle_ap()` 唤醒 idle AP 来捡这个新 Ready 的 task)。这些全是重活(timer_queue.cpp:12 的头注释把这类东西笼统叫「run-queue + kprintf + IPI work of unblock」,但严格读 `scheduler_block.cpp:48-70` 的函数体,`unblock` 自己只做 enqueue + IPI——曾经有过的 per-block `kprintf` 早已按 `scheduler_block.cpp:34` 的注释删掉了,因为它在每次 PTY 读上都打一行,刷爆日志。所以「kprintf」是头注释的笼统说法,不在当前 `unblock` 函数体里)。如果这些重活在 timer spinlock 下跑,有两个麻烦:

1. **拉长 IRQ-off 时间**。timer spinlock 是 `irq_guard`——关着 IRQ。`unblock` 里若再撞上 run-queue 锁竞争、IPI 等待,IRQ-off 窗口被拉长,PIT tick 会丢、网络中断会延迟。
2. **AB-BA 死锁风险**。别处若有「先拿 run-queue 锁、再拿 timer 锁」的路径(理论上有,实际 Cinux 调度器没这种,但纪律要立住),你这边「先 timer 锁、再 run-queue 锁」就和它撞成经典 AB-BA。

所以 tick 把过期 task 收集到栈上 `fired[]`、释放 timer 锁、再循环 `unblock`。这条锁序纪律——**timer_lock → run-queue lock 绝不反着**——是 timer_queue 唯一真正值得学的并发点。头注释([timer_queue.cpp:11-14](../../../kernel/proc/timer_queue.cpp#L11))把这条写得很清楚:

> Lock discipline: the expired set is collected under the spinlock, then `Scheduler::unblock` is called AFTER releasing it, so the (heavier) run-queue + kprintf + IPI work of unblock never runs under the timer lock (lock order is strictly timer_lock -> run-queue lock, never the reverse).

空表的情况也顺带说——`tick` 在表空时就是一次「关 IRQ + 扫 32 个 `armed=false` 的 Entry + 释放」的廉价 no-op,成本可忽略(注释 [scheduler.cpp:375-378](../../../kernel/proc/scheduler.cpp#L375) 写明「Cheap no-op when nothing is armed」)。这就是为什么 timer_queue 不用堆、不用红黑树——线性扫 32 个槽在 PIT tick 频率下成本可忽略,换来的是「无堆下滤、无树旋转、无指针维护」的结构平凡正确。头注释原文([timer_queue.cpp:5-9](../../../kernel/proc/timer_queue.cpp#L5)):

> No heap, no sorted list -- a linear scan of kMaxTimers is cheap at hobby scale (a handful of timed waits at once), and keeps the structure trivially correct.

这是「结构平凡地正确」的支点——选数据结构时,优先选那个让你不用写正确性证明的。

## stats_kthread:周期采样的常驻 kthread + §14 file gate

timer_queue 是给「别人」用的——084 poll 用它挂 deadline。stats_kthread 不一样,它**自己**是个常驻内核线程,目的是给「g++ hello.cpp 卡顿」这类 workload 做 ad-hoc 性能剖析:每 ~1 秒调一次 `dump_memory_stats()`,把 PMM free 页数 / slab 页数 / PageCache 占用 / 累计 #PF / ext2 I/O 打到串口,形成一条曲线。读趋势(哪一秒 cache 暴涨、哪一秒 #PF 飙到 +18272)就能定位根因,而不是猜——这是 Cinux memory 那条「debuggability over perf」铁律的落地。

采样体 `stats_thread_entry`([stats_kthread.cpp:48](../../../kernel/mm/stats_kthread.cpp#L48)):

```cpp
// kernel/mm/stats_kthread.cpp:48-78
void stats_thread_entry() {
    kprintf("[MEM] stats thread entry running\n");
    const bool have_hpet     = cinux::drivers::g_hpet.available();
    uint64_t   next_deadline = cinux::drivers::g_hpet.monotonic_ns() + kStatsIntervalNs;
    int        ticks_since_dump = 0;

    while (true) {
        // yield() hands the CPU to the next band-0 task (init/cc1/g++); the PIT
        // tick preempts it back to us, so we wake ~once per tick (~10 ms) --
        // far below the 1 s dump interval.  See file header for why this is NOT
        // sti/hlt and NOT a lower priority.
        cinux::proc::Scheduler::yield();

        bool due = false;
        if (have_hpet) {
            due = cinux::drivers::g_hpet.monotonic_ns() >= next_deadline;
            if (due) {
                next_deadline += kStatsIntervalNs;
            }
        } else {
            // No HPET: fall back to a tick count (~100 Hz PIT => ~1 s).
            due = (++ticks_since_dump >= 100);
            if (due) {
                ticks_since_dump = 0;
            }
        }
        if (due) {
            dump_memory_stats();
        }
    }
}
```

整个 entry 是个 `while(true)`:每轮先 `Scheduler::yield()` 让出 CPU,醒来后查 HPET `monotonic_ns()` 是否过了 `next_deadline`(初始 = 启动时刻 + 1e9 ns);到了就 `dump_memory_stats()` 并把 deadline 推后 1 秒;没到就回去再 yield。HPET 不可用时退化成数 tick(`++ticks_since_dump >= 100`,~100 Hz PIT 凑 ~1 秒)。

计时基准用 HPET free-running counter,不依赖 PIT IRQ 频率——ns 级粒度。1 秒间隔是粗放有意的:编译几十秒,1 Hz 够画曲线又不刷爆串口(注释 [stats_kthread.cpp:44-46](../../../kernel/mm/stats_kthread.cpp#L44) 写明「Coarse on purpose: a compile takes tens of seconds, so a ~1 Hz sample yields plenty of points without flooding the serial log」)。

为什么这里用 yield 而不是用 timer_queue?这看起来反直觉——既然有了 timer_queue 这么漂亮的定时唤醒原语,为什么 stats_kthread 不 park 自己挂个 1 秒 deadline?下一节专门讲。

spawn 路径 `start_stats_thread`([stats_kthread.cpp:80](../../../kernel/mm/stats_kthread.cpp#L80)):

```cpp
// kernel/mm/stats_kthread.cpp:80-93
void start_stats_thread() {
    // priority 0 = the TaskBuilder default, same band as kernel_init and the
    // fork'd user processes.  See file header: lower starves, higher skews.
    auto* t = cinux::proc::TaskBuilder()
                  .set_entry(stats_thread_entry)
                  .set_name("mm_stats")
                  .set_priority(0)
                  .build();
    if (t != nullptr) {
        cinux::proc::Scheduler::add_task(t);
        kprintf("[MEM] periodic stats thread started (%llu ms interval)\n",
                static_cast<unsigned long long>(kStatsIntervalNs / 1'000'000));
    }
}
```

`TaskBuilder().set_entry(...).set_name("mm_stats").set_priority(0).build()`——`build()` 返 `nullptr` 时静默不 `add_task`、不 panic(内存紧张建不出 task 就算了,采样是 ad-hoc 增强不是关键路径)。`set_priority(0)` 进 band 0——下一节讲为什么必须 0。

`init.cpp` 在 `launch_userspace` 之前无条件调它([init.cpp:155-158](../../../kernel/proc/init.cpp#L155)):

```cpp
// kernel/proc/init.cpp:155-158
// B1 gcc-stutter profiling: spawn the periodic memory-stats kthread.  No-op
// when CINUX_STATS_KTHREAD=OFF (stub); prints a 1 Hz PMM/slab/PageCache/#PF
// curve to the serial log when ON, for narrowing gcc/g++ compile-stutter.
cinux::mm::start_stats_thread();
```

注意这行**没有任何 `#ifdef` 包裹**——OFF 时这行调的是 stub 的空函数,零成本零回归。这就是 §14 file gate 的精髓。

### §14 file gate:CMake 选链,源码零 #ifdef

CMake option 在 [options.cmake:44](../../../cmake/options.cmake#L44):

```cmake
# cmake/options.cmake:44
option(CINUX_STATS_KTHREAD "Spawn periodic 1 Hz memory-stats kthread for ad-hoc profiling" OFF)
```

默认 OFF。选链逻辑在 [CMakeLists.txt:13-24](../../../kernel/mm/CMakeLists.txt#L13):

```cmake
# kernel/mm/CMakeLists.txt:13-24
# B1 gcc-stutter profiling: §14 file gate. ON links the real periodic stats
# kthread (stats_kthread.cpp); OFF links the empty stub (stats_kthread_stub.cpp)
# so init.cpp's start_stats_thread() call resolves either way -- no #ifdef in
# source. The PF counter + dump_memory_stats PF line are unconditional (built
# into page_fault.cpp / diagnostics.cpp); only the periodic thread is gated.
if(CINUX_STATS_KTHREAD)
    target_sources(big_kernel_common PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/stats_kthread.cpp)
else()
    target_sources(big_kernel_common PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/stats_kthread_stub.cpp)
endif()
```

ON 链 `stats_kthread.cpp`(真实现),OFF 链 `stats_kthread_stub.cpp`([stats_kthread_stub.cpp:15](../../../kernel/mm/stats_kthread_stub.cpp#L15)):

```cpp
// kernel/mm/stats_kthread_stub.cpp:13-17
namespace cinux::mm {

void start_stats_thread() {}

}  // namespace cinux::mm
```

空函数,签名一模一样。链接器替 `init.cpp` 选 TU——`init.cpp` 完全不用关心开关,读起来和普通函数调用一样干净。这套模式和 `usb_stub.cpp`(USB 编译关时的空 init)/ `tlb_drain_stub`(TLB drain kthread 关时的空 spawn)是同一套——Cinux 把「可选增强」收进 file gate 的标准姿势。

更细的一点:`PF` 计数器([page_fault.cpp:62](../../../kernel/arch/x86_64/page_fault.cpp#L62))和 `dump_memory_stats` 的 #PF 行([diagnostics.cpp:54](../../../kernel/mm/diagnostics.cpp#L54))是**无条件编的**——panic dump 也能看 #PF——只有「周期采样线程」被 gate。注释原文([CMakeLists.txt:16-17](../../../kernel/mm/CMakeLists.txt#L16)):「The PF counter + dump_memory_stats PF line are unconditional; only the periodic thread is gated」。理由很实在:panic 时调一次 `dump_memory_stats` 看 #PF 总数是 debuggability 基线,不该被一个 ad-hoc profiling 的开关牵连;而周期采样是「跑 workload 时多打一堆串口」的增强,默认关掉避免污染日常日志。

## band 0 kthread 不能 sti/hlt:yield 解法(本章明星,串 071)

这是本章最硬的反直觉真坑。源码头注释([stats_kthread.cpp:12-24](../../../kernel/mm/stats_kthread.cpp#L12))把三版尝试讲成一条推理链,咱们逐段拆。

**前提**:Cinux 调度器优先级是「lower runs first」([scheduler.hpp:41](../../../kernel/proc/scheduler.hpp#L41) 头注释:「priority-aware selection (lower Task->priority runs first)」)。`TaskBuilder` 默认 `priority_ = 0`([task_builder.hpp:81](../../../kernel/proc/task_builder.hpp#L81)),被 `/sbin/init` 继承,被 fork/exec 出来的 cc1 继承——所以全系统用户代码都坐 band 0。stats kthread 要采的就是这些 band 0 用户的 workload。

那 stats kthread 自己该坐哪一档?三个候选,前两个各自炸在一个不同维度。

### 症状一:priority 250(近 idle 255)+ yield → 观察者饿死在被观察者手里

第一版尝试给 stats kthread 一个低优先级(比如 250,接近 idle 的 255),心想「它是个后台观测线程,跑得闲一点就行」。

- **症状**:开 `CINUX_STATS_KTHREAD=ON`、make run、跑 `g++ hello.cpp`,串口只有 `[MEM] stats thread entry running` 那一行启动日志,**整个编译期 0 行 dump**。
- **根因**:「lower runs first」——只要还有任何 band 0 任务 Ready,CPU 就先跑 band 0,band 250 的 stats kthread 永远轮不到。而 `g++ hello.cpp` 恰恰是个 CPU-bound workload——cc1/cc1plus 几乎全程占着 CPU。结果是「**观察者饿死在被观察者手里**」——你专门为了观察编译卡顿起的线程,被编译本身饿死了,一行采样都采不到。
- **定位**:看串口 dump 行数——启动后到编译结束,`[MEM] === t=...` 那行一次都没出。
- **修复**:提到 band 0,和被观测对象同档。
- **防复发**:头注释([stats_kthread.cpp:16-18](../../../kernel/mm/stats_kthread.cpp#L16))写明「Lower (e.g. 250, near idle 255) STARVES under a CPU-bound compile -- the exact workload we want to observe -- so no samples ever land」。

### 症状二:priority 提高 → 抢占 user code,自欺欺人

第二个候选是反过来——给 stats kthread 一个**比 user code 更高**的优先级(数字比 0 还小,或抢在 band 0 前),心想「让它优先跑,采样及时」。

这条没真在 Cinux 上跑过(头注释是一句话带过的「Higher would preempt user code and skew the very numbers we measure」),但逻辑很清楚:

- **根因**:你专门起一个线程去量 user code 的内存行为,结果这个线程自己优先级比 user code 高——user code 没跑满就被你抢占、你 `dump_memory_stats` 时 user code 没在干活——你采的是「**被你干扰过的数**」,自欺欺人。这违反了观测的基本伦理:**观测者不能干扰被观测对象**。量子力学的测不准原理在性能剖析里有一个朴素的工程版本——你的采样线程不能扭曲它要量的东西。
- **修复**:不能更高,必须和 user code 同档。

所以 stats kthread 必须 band 0——既不能低(饿死)、也不能高(扭曲)。但 band 0 内「等 1 秒」还有第三个坑。

### 症状三:band 0 + sti/hlt → gate 卡在第一次 fork(本章最坑)

第三版尝试是 band 0,但「等 1 秒」用 `sti; hlt`(开中断然后 halt 等 IRQ)——心想「halt 省 CPU,tick IRQ 唤醒自己」。

- **症状**:`CINUX_STATS_KTHREAD=ON` make run,系统启动到 busybox init 第一次 fork 就**卡死**——串口停在 init fork 那条日志,#PF 全程 +0(根本没采到样)。
- **根因**:`hlt` 在一个 band-0 线程里是灾难。`hlt` 让 CPU 停下来等中断,IRQ0(PIT tick)来了唤醒——但唤醒后**当前 quantum 没耗尽**,调度器 tick 里 `task_tick` 只记账不强制切换(注释 [scheduler.cpp:381-385](../../../kernel/proc/scheduler.cpp#L381) 写明「context_switch.S enables IF before jumping to the next task; doing that while the old irq0 frame is still live lets the PIT re-enter recursively. Real preemption needs a return-from-IRQ resched point」——这条注释确立的是「tick 不内联切」这个非抢占模型;而「hlt 后的线程会被反复选中」这个具体后果是 CinuxOS dev note 定位的:`stats 在时间片内 hlt,tick IRQ 唤醒后继续 stats(时间片未耗尽),init/child 永远 Ready 等 → 饿死`),于是 CPU 又选回 priority 0 最高的 stats kthread 自己——它再 `hlt`、再被 tick 唤醒、再选自己......init / fork 出来的 child 永远是 Ready 状态等不到 CPU。这跟 071 章那个 sti/hlt #DF 坑是**同族**(都是 Cinux 里 sti/hlt 用错上下文),但**机制不同**:071 章是 syscall 上下文里 `sti` 打开一个窗口,LAPIC 时钟中断在那个窗口抢 `%gs:0` 栈上的 syscall 陷阱帧,sysretq 弹花就 #DF(根子是**陷阱帧损坏**);这里是 band-0 kthread 里 `hlt`,tick IRQ 唤醒后 quantum 没耗尽,调度器重选自己,把同级 init/child 饿死(根子是**非抢占 + 同优先级重选**)。两者的共性只到「都是在错误上下文用 sti/hlt」这一层,不要把两个不同的根机当成同一个。
- **定位**:看 #PF 卡在 +0、gate 卡在 busybox init 第一次 fork——典型的「观测线程垄断了 CPU、被观测对象跑不动」。
- **修复**:不 `hlt`,改 `yield`。
- **防复发**:头注释([stats_kthread.cpp:19-22](../../../kernel/mm/stats_kthread.cpp#L19))明确「do NOT sti/hlt: halting inside a band-0 thread makes the tick IRQ resume us (quantum not exhausted) and every other band-0 task (init / fork / exec) waits forever -- the gate freezes at the first fork with #PF stuck at +0」。

### 正解:band 0 + yield——醒得很勤,干活很稀

正解是 band 0(priority 0)+ `yield`:

```cpp
// kernel/mm/stats_kthread.cpp:54-59(主循环开头)
while (true) {
    // yield() hands the CPU to the next band-0 task (init/cc1/g++); the PIT
    // tick preempts it back to us, so we wake ~once per tick (~10 ms) --
    // far below the 1 s dump interval.  See file header for why this is NOT
    // sti/hlt and NOT a lower priority.
    cinux::proc::Scheduler::yield();
    // ... 查 HPET deadline,到了才 dump ...
}
```

`yield` 主动把 CPU 让给下一个 band-0 task(init/cc1/g++)——它干活,你睡;PIT tick(~10ms 后)再切回 stats,你醒一次查一次 HPET deadline(几 ns 的活儿),没到 1 秒就回去再 yield。这是「**醒得很勤(约 100Hz)但干活很稀(1Hz dump)**」的两级节流:

- 不饿死编译——yield 让出 CPU 给 cc1,cc1 真在跑。
- 不扭曲测量——yield 是协作式让出,你 dump 的瞬间 user code 刚跑完一个 quantum,采的是真实的 workload 状态。
- 不垄断 CPU——yield 不 halt,不把 CPU 交给中断反复选自己。

`yield` 在这里同时干两件事:**让出 CPU 给被观测对象** + **等下一个 tick**。一个原语两个职责,这就是为什么 stats_kthread 没用 timer_queue——timer_queue 的 park 会把 task 翻 Blocked 睡死,错过中间所有的采样窗口(它要的是「醒 100 次只 dump 1 次」,不是「睡 1 秒醒 1 次」);而且 park 的 task 在 band 0 里同样会撞上「谁来叫醒它」的问题——叫醒它的 tick IRQ 在 band 0 上下文,timer_queue 的 `unblock` 把它翻 Ready 后,调度器选谁还是 priority 说了算。所以 stats_kthread 是**另一种定时范式**:不是「到点叫醒」(timer_queue),而是「勤醒懒干活」(yield + 查 deadline)。

教学点:在协作式/带优先级的调度器里,后台观测线程的调度不是随便挑个原语——它必须够高优先级(不饿死)、不能更高(不扭曲)、且不能 halt(不垄断剩余时间片)。`yield` 是唯一同时满足这三个约束的姿势。

## dump_memory_stats:四条正交维度 + PF delta(rate 而非 total)

`dump_memory_stats`([diagnostics.cpp:26](../../../kernel/mm/diagnostics.cpp#L26))把「内存压力」拆成 PMM/slab/PageCache/#PF 四条正交数据维度(timestamp 行只是时间轴不算数据维度),每次同步报全,后来追加第 5 条 ext2 I/O,一条串口日志就是曲线上的一个点:

```cpp
// kernel/mm/diagnostics.cpp:26-74(节选关键行)
void dump_memory_stats() {
    const uint64_t free_pages  = g_pmm.free_page_count();
    const uint64_t total_pages = g_pmm.total_page_count();
    const uint64_t pf_total    = pf_count();

    // Boot-relative timestamp so the periodic stats log is a curve with a real
    // time axis (HPET monotonic ns) ...
    const uint64_t now_ns =
        cinux::drivers::g_hpet.available() ? cinux::drivers::g_hpet.monotonic_ns() : 0;
    kprintf("[MEM] === t=%llu.%03u s ===\n", ...);

    kprintf("[MEM] PMM:       %u / %u pages free ...\n", ...);          // 物理内存余量
    kprintf("[MEM] Slab:      %u slab pages mapped\n", ...);            // 内核对象分配器
    kprintf("[MEM] PageCache: %u cached (%u hits / %u misses)\n", ...); // 文件页缓存

    // Delta vs the previous dump so the periodic stats thread's log shows a PF
    // rate, not just a monotonic total.  static: dump_memory_stats has no
    // concurrent callers in practice (panic once + the single stats thread).
    static uint64_t last_pf = 0;
    kprintf("[MEM] #PF:       %u total (+%u since last dump)\n", ...);
    last_pf = pf_total;

    // B2.5: cumulative ext2 read I/O (count + bytes + wall time) with delta ...
    static uint64_t last_io_ns    = 0;
    static uint64_t last_io_reads = 0;
    // ... ext2_read_ns() / ext2_read_count() / ext2_read_bytes() + delta ...
}
```

五条行的设计:

- **`[MEM] === t=N.NNN s ===`**:HPET 时间戳。让曲线有真实时间轴,能把 [MEM] 样本对齐到 workload 阶段(比如 g++ 编译窗口)。HPET 不可用时打 0。
- **`[MEM] PMM: free/total pages`**:物理内存余量。看是不是快 OOM。
- **`[MEM] Slab: total_slab_pages`**:内核对象分配器占页。看是不是泄漏膨胀。
- **`[MEM] PageCache: cached_pages + hit/miss`**:文件页缓存占用与命中率。grow-only bug 在这里显形。
- **`[MEM] #PF: 累计缺页数 + (+Δ since last dump)`**:这是关键设计——用 `static uint64_t last_pf`([diagnostics.cpp:53](../../../kernel/mm/diagnostics.cpp#L53))跨调用算 delta。
- **`[MEM] I/O: ext2 read 累计 reads/bytes/ms + delta`**:后来追加的第 5 行,把「内存压力」扩展到「I/O 时间归属」——卡顿到底是 demand-paging 的 I/O 时间,还是 syscall/TCG 翻译开销。

### profiling 靠趋势不靠单点——static last_pf 是核心设计

#PF 用 delta 而不是单调总量,是这套设施的灵魂。dev note 那条 31s g++ 编译曲线里,PF 累计 31117 是个无意义的总数——「累计缺页 31117 次」告诉你什么?什么都没告诉你。但 sec 20 的 **+18272** 一眼定位到 cc1plus 加载 libstdc++ 的 demand paging 爆发——「这一秒发生了 18272 次缺页」才是诊断信号。单点采样只能告诉你「现在多少」,趋势采样才能告诉你「**这一秒发生了什么**」。

这条铁律:**任何 ad-hoc profiling 设施的 counter 必须配 delta**,否则曲线是平的、读不出工作负载阶段。注释([diagnostics.cpp:50-52](../../../kernel/mm/diagnostics.cpp#L50))把这个 static 的不变量写得很清楚:

> Delta vs the previous dump so the periodic stats thread's log shows a PF rate, not just a monotonic total. static: dump_memory_stats has no concurrent callers in practice (panic once + the single stats thread).

「no concurrent callers」是 `static` 安全的前提——panic 时调一次 + 单 stats 线程调,两者不会并发,所以 `static last_pf` 不需要锁。这条不变量要记住:如果哪天加了第二个调用者(比如某个 syscall 也调 `dump_memory_stats`),这个 `static` 就 race 了,得加锁或者改成 per-caller state。

### PF 计数器本身:多核下原子是底线

PF 计数器在 [page_fault.cpp:62](../../../kernel/arch/x86_64/page_fault.cpp#L62):

```cpp
// kernel/arch/x86_64/page_fault.cpp:59-75
// Cumulative #PF count for ad-hoc profiling (B1 gcc-compile-stutter).  handle_pf
// runs at IF=0 but multiple CPUs can fault concurrently under -smp 2, so a plain
// ++ would race; an atomic add is cheap and correct.
uint64_t g_pf_count = 0;
}  // namespace

// Total #PF since boot (atomically bumped by handle_pf).  Defined next to the
// counter; declared in fault_diag.hpp so dump_memory_stats can read it without
// dragging in the whole PF handler.
uint64_t pf_count() {
    return __atomic_load_n(&g_pf_count, __ATOMIC_RELAXED);
}

extern "C" {

void handle_pf(InterruptFrame* frame) {
    __atomic_fetch_add(&g_pf_count, 1, __ATOMIC_RELAXED);
    // ... 后续 fault 处理 ...
```

这里有个反直觉点:page fault 在 IF=0 的 fault 上下文里走,看起来天然单线程——一个核在处理 fault 时,这个核的中断是关的。但 CinuxOS 跑 `-smp 2` 时**两核可以同时 fault**——两个核各自跑 cc1、各自 demand-page、各自进 `handle_pf`。plain `++g_pf_count` 在两核并发下会 race 丢计数(经典 read-modify-write 竞争)。所以用 `__atomic_fetch_add(..., __ATOMIC_RELAXED)`——RELAXED 够用,因为只要计数、不要顺序(不需要 #PF 计数和别的内存操作有 happens-before 关系)。

教学:**「中断关闭」不等于「无并发」**。单核下 IF=0 确实串行化了 fault 处理;但多核下,每个核有自己的 IF,你这核关中断不影响别核。多核下原子操作是底线,哪怕看起来「天然单线程」的路径。

## 验证:timer 被 084 间接证、stats kthread 靠运行曲线

这两个原语都属于「真调度难单测」一类——验证策略不同,但共同点是**不写直接单测、靠集成路径 + 运行观察**。这一节诚实标注边界,不拔高。

### timer_queue:靠 084 间接证,没有独立单测

grep `kernel/test/` 没有 `test_timer_queue.*`——整棵树只有 084 的 `test_poll_finite_timeout_parks_then_returns_zero`([test_poll.cpp:191-204](../../../kernel/test/test_poll.cpp#L191)):

```cpp
// kernel/test/test_poll.cpp:191-204
/// A finite-timeout poll on an empty pipe PARKS (registered on the read wait
/// queue + the timer-wake) and returns 0 once the timer fires -- the real
/// blocking path, not a yield spin.  Proves the timer-wake end-to-end.
void test_poll_finite_timeout_parks_then_returns_zero() {
    int rfd, wfd;
    make_pipe(rfd, wfd);

    uint16_t rv = 0xFFFF;
    int64_t  r  = poll_one_timeout(rfd, kPollIn, 30, rv);
    TEST_ASSERT_EQ(r, 0);  // nothing became ready -> timeout
    TEST_ASSERT_EQ(rv, 0);

    close_fds(rfd, wfd);
}
```

它 poll 一根空 pipe、超时 30ms、期望 `r==0` 且 `revents==0`。它没点名 `timer_queue`,但「30ms 后真能返回」**只可能是** timer_queue 在 tick 里唤醒了被 park 的任务——这是 timer_queue 端到端的活证据。整条链是:

1. `poll_one_timeout` 进 `do_poll_core` 的 park 块(`poll_core.cpp:180`)。
2. `InterruptGuard` 关 IRQ → `prepare_to_wait` 翻 Blocked → `register_all` 挂 read 队列 → `timer_queue_arm(self, deadline)`(`:188`)挂 30ms deadline。
3. `schedule_blocked`(`:204`)真切走。
4. PIT 驱动的 `Scheduler::tick`(`scheduler.cpp:367` 定义)每次中断在 `:379` 调 `timer_queue_tick`,30ms 后扫到 deadline 过期,把 task 收进 `fired[]`、锁外 `Scheduler::unblock`。
5. poller 醒来,`detach_all` 撤销 fd 注册、`timer_queue_disarm`(`:207`)清自己的槽位。
6. 回 for 顶 Pass 1 重扫,空 pipe 没就绪、deadline 已过,返回 0、revents=0。

这条链跑通 = timer_queue 端到端验过。所以本章引用 timer_queue 时标「被 084 间接验证」,不是「有单测」——这是诚实的措辞。

### stats_kthread:靠 ON + make run + 串口曲线,没有单测

stats_kthread 依赖真调度器循环 + 真时间流——yield 让 CPU、PIT tick 切回、HPET 判 1s。单测要造真调度器 + 时间快进,工程上不值。验证路径是运行态:

1. `cmake -DCINUX_STATS_KTHREAD=ON` 配 make。
2. `make run`,跑真实 workload(典型 buildroot rootfs 里 `g++ hello.cpp`)。
3. 看串口每隔 ~1 秒出一组 `[MEM] === t=N.NNN s ===` + PMM/Slab/PageCache/#PF/I/O 五行。

CinuxOS 开发笔记给过一条 31s g++ 编译曲线就是这种验证的产物——PMM 始终 ~2.08M 页(8GB 充裕,sec 0 `2088396` / sec 30 `2086187`)、Cache 才涨到 1767 页(~7MB,sec 30)、PF 爆发集中在 sec 8/13/20(对应 cc1/cc1plus 加载大库,sec 20 `+18272` 是峰值),诊断结论是「内存子系统不是瓶颈」,卡顿要往 QEMU TCG 翻译开销 / disk I/O / 热 syscall 找。(那条曲线是 ext2 I/O 行还没加进 dump 之前采的——当时每次 dump 4 行 [MEM];当前源码每次 dump 6 行,同样 31s 复现会是 ~186 行。但 per-sample 的数字——PMM free、Cache、PF total、PF delta——不受每 dump 行数影响,仍对得上。)

**注意**:默认 OFF 跑过 2290 PASS / 0 FAIL(dev note 数据)只验「gate 不破坏 kernel」(stub 链零回归),**不验「kthread 真能周期采样」**——后者要 ON + make run。这两条要分开标,别把前者当后者。

还有个容易混的点:`test_memory_stats.cpp`([test_memory_stats.cpp:21](../../../kernel/test/test_memory_stats.cpp#L21)):

```cpp
// kernel/test/test_memory_stats.cpp:21
void test_dump_runs_and_reports() {
    dump_memory_stats();  // observed on serial; must not fault
    TEST_ASSERT_GT(g_pmm.total_page_count(), 0ULL);
    TEST_ASSERT_GT(g_pmm.free_page_count(), 0ULL);
}
```

它只调一次 `dump_memory_stats` 断言不 fault、PMM total/free > 0。这是 FO(Foundation)基础设施的测试——测的是 `dump_memory_stats` 这个函数本身能跑、PMM 计数器有值——**不是 stats_kthread 的证据**。别把它当成「stats_kthread 有单测」。

### 两条 gate 分开

这两个原语的 gate 状态也不一样,要分清:

- **timer_queue**:没 gate,永远编进 kernel(`timer_queue.cpp` 在 `kernel/proc/CMakeLists` 里无条件链),被 084 poll 吃。
- **stats_kthread**:§14 file gate,默认 OFF。OFF 时链 stub,ON 时链真实现。

## 这章没做的

诚实的边界清单:

1. **没在本机跑 `cmake -DCINUX_STATS_KTHREAD=ON` + `make run` 复现 31s 曲线**——曲线数字(2088396 / 1767 / 31117 / +18272)和 2290 PASS/0 FAIL 全部来自 CinuxOS dev note。本章只核实 Book 侧源码接线与该 note 描述一致——文件齐全、行号对得上、调用链闭合。读者若在本机 ON + make run,数字会因 rootfs / QEMU 配置略有差异。
2. **没核 timer_queue 的 SMP 安全细节**——表是全局单例 `g_entries[32]`,AP 也调 `Scheduler::tick`(AP 起来后同样走 tick 路径),多核 tick 都拿同一 `g_lock` spinlock,理论安全,但没压力测试。084 的单次 30ms 超时不触发 `kMaxTimers` 表满、不触发多核并发 arm 等边界。
3. **timer_queue 的并发正确性(锁顺序、IRQ safety)是基于源码读出的论断,无压力测试佐证**——「timer_lock → run-queue lock 绝不反着」这条纪律是源码注释和实现结构推出的,没有专门的并发测试守住。
4. **stub 路径下 PF counter / dump #PF 行「无条件编」基于 CMakeLists 注释**——`diagnostics.cpp` 确实无 gate(全文读了一遍没 `#ifdef`),`page_fault.cpp` 只读了 `:55-89` 区段确认 `g_pf_count` 和 `handle_pf` 的原子操作,没逐行扫整个文件确认完全没有 `#ifdef` 包裹(但从结构看,PF 计数是 fault handler 的基础逻辑,被 gate 反而不合理)。
5. **nanosleep 未迁仍 yield-poll**——`timer_queue.hpp` 头注释([timer_queue.hpp:5-10](../../../kernel/proc/timer_queue.hpp#L5))提了 nanosleep 是 timer_queue 的目标用户,但 `sys_nanosleep.cpp:50-55` 还在 yield 自旋。**以 `.cpp` 为准**,nanosleep 这条现在其实没生效——`timer_queue` 真正的用户是 084 poll。
6. **stats_kthread 没用 timer_queue**——它是更早的 yield-poll 范式,band-0 旁观采样用 `yield` 同时承担「让 CPU 给被测对象」和「等 tick」两职责,park 反而不合适(会睡死、错过采样窗口)。这是两套不同场景的定时范式,本章只点出区别,不展开 timer_queue 改造 stats_kthread 的可行性(那是个 DEBT,没人做过)。
7. **HPET 依赖**——不可用时 stats_kthread 退化成数 tick(`++ticks_since_dump >= 100`),精度从 ns 掉到 ms 级(实际是 ~1 秒但抖动大)。本章不展开 HPET 不可用场景的实测。
8. **`dump_memory_stats` 的 `static last_pf`/`last_io` 依赖「无并发调用者」不变量**(panic 一次 + 单 stats 线程)——这条不变量目前成立,但如果哪天加了第二个调用者就 race。本章只标注,不改造。

## 小结

这章核实了两件内核基础设施在 Book 真能用,主题是「教程即验证」。

**timer_queue** 补的是调度器「到点自动唤醒」缺失的半边——084 poll 用它的 arm/disarm 接口真 park 了一个有限 timeout 任务。内部是一个容量 32 的固定表,线性扫,O(n) 在 PIT tick 频率下成本可忽略,换来「无堆下滤、无树旋转」的结构平凡正确。三个接口:`arm` 关 IRQ 拿锁、先替换已有 arm、再找空槽、表满返 false 让调用方降级;`disarm` 醒来清槽防 stale;`tick` 由 `Scheduler::tick` 每次中断调,锁内收集过期 task 到栈上 `fired[]`、锁外才 `unblock`——这条锁外唤醒是 timer_queue 唯一真正值得学的并发点,锁序纪律 timer_lock → run-queue lock 绝不反着,否则 AB-BA 死锁。

**stats_kthread** 是周期采样的常驻 kthread,每秒 `dump_memory_stats` 打串口曲线。本章的明星是它的调度难点——围绕「不饿死、不扭曲、不垄断 CPU」三个角的不可能三角,两版实测失败 + 一版逻辑排除推出来的正解:

- priority 250 → 观察者饿死在被观察者手里,0 行 dump;
- priority 提高 → 抢占 user code 扭曲测量,自欺欺人;
- band 0 但 sti/hlt → tick 唤醒后 quantum 没耗尽继续选自己,init/child 永远 Ready,gate 卡在第一次 fork、#PF 全程 +0(与 071 章 sti/hlt 坑同族,机制不同:071 是陷阱帧损坏 #DF,这里是同级任务饿死)。

正解是 band 0 + yield——「醒得很勤(约 100Hz)但干活很稀(1Hz dump)」的两级节流,`yield` 同时承担「让 CPU 给被测对象」和「等下一个 tick」两件事。这是协作式/带优先级调度器里后台观测线程的正确姿势。

**§14 file gate** 把 gate 放在「整个周期线程」这一层——ON 链真实现、OFF 链空 stub,源码零 `#ifdef`,`init.cpp` 无条件调 `start_stats_thread()`,读起来和普通函数调用一样干净(和 `usb_stub`/`tlb_drain_stub` 同一套模式)。更细的一点:PF 计数器和 dump 的 #PF 行是无条件编的——panic 时也能看 #PF——只有「周期采样」这个 ad-hoc profiling 增强被 gate。

验证边界诚实标:timer_queue 没有独立单测,靠 084 的 `test_poll_finite_timeout_parks_then_returns_zero` 间接证 arm→park→tick 唤醒→disarm 整链;stats_kthread 没有单测,靠 ON + make run + 串口曲线(CinuxOS dev note 的 31s g++ 曲线是实测,Book 本地未重跑;dev note 的「126 行」是 ext2 I/O 行还没加进 dump 之前的 4 行/dump 计数,当前源码每次 dump 6 行,同样 31s 复现应是 ~186 行);`test_memory_stats.cpp` 测的是 `dump_memory_stats` 这个 FO 组件不是 kthread 本身。两条 gate 也分开——timer_queue 没 gate 永远编,stats_kthread 是 §14 file gate 默认 OFF。这些都不拔高,如实说。
