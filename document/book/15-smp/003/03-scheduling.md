---
title: 03 · 多核调度:共享 run queue 与 reschedule IPI
---

# 多核调度:共享 run queue 与 reschedule IPI

## 多核调度:共享 run queue 怎么不撞车

到这里两个核都 online 了,但"多核调度"真正难的地方才刚开始。核心问题:**两个核从同一个 run queue 里挑任务,怎么保证不会挑到同一个?**

第一件事,先给每个核一个**专属的 idle 任务**(`idle_tasks_[kMaxCpus]`)。为什么不能两个核共享一个 idle?因为 context_switch 会切到 idle 的 ctx、用 idle 的栈。两个核要是共享一个 idle,就会两个核同时切到同一个 ctx、踩同一份 16 KB 栈——必炸。所以 BSP 用 `idle_tasks_[0]`(老的 `idle_entry`,while-hlt,PIT 驱动),每个 AP 用 `setup_ap_idle` 造自己的(入口 `ap_idle_entry`)。

第二件事,**运行中的任务绝不许留在 run queue 里**。看 `RoundRobin::pick_next`(`roundrobin.cpp`):它选出最高优先级的任务后,**把它从队列里移除**(`remove_at_locked`),不是轮转着再塞回队尾。注释把这个纪律说得很直白:一个正在跑的任务要是还留在共享队列里,第二个核的 `pick_next` 就可能再选中这个已经 Running 的任务,两个核 context-switch 到同一份栈/ctx 上。

配套的纪律在 `schedule()`(`scheduler.cpp`)里:prev(刚跑完、要让出的任务)如果还是 Ready,才 re-enqueue 进队列;`pick_next_task` 再把 winner **移除**。于是队列里**永远只有"可跑但没在跑"的任务**,任何时刻一个任务至多被一个核选中。单核的轮转语义也没丢:schedule 先把 yielding 的 prev 塞回队,再 pick,一个光杆任务会挑回自己(`next==prev` 路径)继续跑。

## reschedule IPI:把闲置的 AP 拽起来干活

现在 AP 的 idle 不再是死 halt。看 `ap_idle_entry`(`scheduler.cpp`):

```cpp
while (true) {
    if (has_runnable_task()) {   // 纯 peek,不 dequeue
        schedule();              // 有活,从共享队列拉一个跑
    }
    __asm__ volatile("sti; hlt");  // 没活,开中断 + halt
    __asm__ volatile("cli");
}
```

AP 空闲就 `sti;hlt` 睡。当别处有新任务可跑了(比如某个任务被 unblock、或 fork 出新任务),谁来叫醒这个 AP?靠 **reschedule IPI**。向量选 `0xE0`(`smp.hpp`),特意避开 PIC IRQ 区(0x20-0x2F)、伪中断(0xFF)、sigreturn 陷阱(0x80)。`wake_idle_ap` 给每个 online AP 发一炮 IPI——**best-effort,不精确**:多发给一个正忙的 AP 无害,它的 idle 循环重新查一遍队列、空的话再 halt 就是。这换来的是不用维护一个「哪个核正 idle」的精确标志(那个标志的 BSP 读 / AP 写自己又要费心 race)。

这里有个经典的 **lost-wakeup 窗口**要堵:AP 查了队列(空)、正要 `sti;hlt`,就在这当口,另一个核往队列里塞了个任务并(按理)该发 IPI——可 AP 还没 hlt,IPI 白发,AP 睡死。堵法是 `has_runnable_task()` 这个**纯 peek**(不 dequeue,各 class 的 `is_empty()` 自己加锁),在 `cli` 下查:查完才 `sti;hlt`。而塞任务的 `add_task`/`unblock` 发的正是这发 IPI——只要任务在查和 hlt 之间入队,IPI 就会把 AP 从 hlt 拽起来重查,不会漏。
