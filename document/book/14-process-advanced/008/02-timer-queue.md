---
title: 02 · timer_queue 的内部:固定表 + tick 扫过期(承接 084 的用法)
---

# timer_queue 的内部:固定表 + tick 扫过期(承接 084 的用法)

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

## 三个接口逐个看

`timer_queue_arm`([timer_queue.cpp:54](../../../kernel/proc/timer_queue.cpp#L54)):

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

## 锁外唤醒的锁序纪律——本章第一个真值得学的并发点

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
