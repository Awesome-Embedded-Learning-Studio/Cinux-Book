---
title: 04 · 统一 park:关中断 + register_all + schedule_blocked
---

# 统一 park:关中断 + register_all + schedule_blocked

Pass 1 全没就绪、且没超时,就进统一 park 块(`poll_core.cpp:180`)。这是防 lost-wakeup 的核心。块全文如下(`:180` 起,到 `:214` 的 `return -cinux::kEintr;` 结束):

```cpp
// kernel/syscall/poll_core.cpp:180 (park 块全文)
{
    // IRQs off across prepare_to_wait + registration + arm: no tick can
    // preempt a Blocked task before it is on a queue / armed (lost
    // wakeup), and the waiter lands on every queue before IRQs return.
    cinux::proc::InterruptGuard ig;
    cinux::proc::Scheduler::prepare_to_wait(self);  // state -> Blocked
    became_ready = register_all(pfds, nfds, self, &registered);
    if (!infinite) {
        armed = cinux::proc::timer_queue_arm(self, deadline);
    }
    // Sleep unless a fd is already ready, or (infinite with nothing that
    // could ever wake us -- avoid a permanent hang).
    will_sleep = !became_ready && (registered || armed);
    if (!will_sleep) {
        self->state = cinux::proc::TaskState::Running;  // cancel the prepare
    }
}  // IRQs restored
if (!will_sleep) {
    if (became_ready) {
        detach_all(pfds, nfds, self);
        continue;  // pass 1 will report the ready fd
    }
    return 0;  // infinite, nothing registered & no timer -> no wake source
}
cinux::proc::Scheduler::schedule_blocked();  // switch out; woken by fd or timer
detach_all(pfds, nfds, self);
if (armed) {
    cinux::proc::timer_queue_disarm(self);  // free the timer slot
}
if (signal_deliverable_pending(self)) {
    return -cinux::kEintr;  // POSIX poll 可被信号中断
}
// loop: re-scan (a fd is ready, or the timeout expired)
```

四步解读:

**第一步,关中断。** `InterruptGuard ig` 把本地 CPU 的 IRQ 关掉。这一步是为了防止时钟 tick 在「prepare_to_wait 翻 Blocked」和「register_all 入队」之间打断——如果这中间被 tick 抢走、切到别的任务,这个 Blocked 任务还没挂上任何队列,谁也叫不醒它,睡死。

**第二步,prepare_to_wait。** 把 `self->state` 翻成 Blocked(`scheduler_block.cpp:72-81`)。注意此刻 task 还在跑——只是 state 标记变了,等到下面的 `schedule_blocked` 才真切换出去。

**第三步,register_all + timer_queue_arm。** `register_all`(`poll_core.cpp:97-123`)把同一个 `self` 挂进每一个可阻塞 fd 的等待队列,顺带在 fd 自己的锁下**重新查就绪**(`became_ready`):

```cpp
// kernel/syscall/poll_core.cpp:97
bool register_all(kpollfd* pfds, uint64_t nfds, cinux::proc::Task* waiter, bool* registered) {
    *registered = false;
    bool ready  = false;
    for (uint64_t i = 0; i < nfds; ++i) {
        FdRef    r      = resolve_fd(pfds[i].fd);
        uint32_t wanted = static_cast<uint16_t>(pfds[i].events) | always_bits;
        bool     reg    = false;
        uint32_t raw    = 0;
        switch (r.kind) {
        case FdKind::kInode:
            raw = r.inode->ops->poll_events(r.inode, waiter, &reg);  // 这次传真 waiter
            break;
        case FdKind::kConsole:
            raw = cinux::drivers::console_tty().poll_events(waiter, &reg);
            break;
        default:
            continue;
        }
        if (reg) {
            *registered = true;  // 至少一个 fd 登记了 waiter
        }
        if ((raw & wanted) != 0) {
            ready = true;  // 至少一个 fd 此刻就绪
        }
    }
    return ready;
}
```

这里有个反直觉的细节——`became_ready` 二次确认就绪。为什么 Pass 1 已经查过没就绪,这里还要再查?因为从 Pass 1 到关 IRQ 之间,可能恰好有数据到达(producer 在这窗口里写了)。这时取消 prepare(state 改回 Running)、`detach_all`、`continue` 让 Pass 1 报出就绪 fd,**而不是傻睡**。这是 prepare_to_wait 契约的另一半——「挂队列入原子窗口内重查就绪」,避免「查完没就绪 → 准备睡 → 此时数据到了 → 你却睡了」的丢唤醒。

有限 timeout(`!infinite`)再 `timer_queue_arm(self, deadline)`(`poll_core.cpp:188`)挂一个绝对截止时间到 timer queue。这样 poller 同时挂在「N 个 fd 队列」和「timer queue」两个唤醒源上——任一 fd 来数据、或 timer 到点,都会调 `Scheduler::unblock`。

**第四步,schedule_blocked。** 出 InterruptGuard 窗口(IRQ 恢复)后,`schedule_blocked`(`scheduler_block.cpp:83-110`)真正切走。任一唤醒源(fd 的 `wake_one` 或 timer tick)调 `Scheduler::unblock(self)` 把 state 翻回 Ready、塞进运行队列,任务就会被重新调度上 CPU。

这里有个「避免永久挂死」的守护(`poll_core.cpp:192-203`):

```cpp
will_sleep = !became_ready && (registered || armed);
if (!will_sleep) {
    self->state = cinux::proc::TaskState::Running;  // 撤销 prepare
}
// ...
if (!will_sleep) {
    if (became_ready) { detach_all(...); continue; }  // 有就绪 -> 回 for 顶报
    return 0;  // infinite 且没注册且没 arm timer -> 无唤醒源,不睡
}
```

如果 `infinite` 超时 + 没有任何 fd 注册成功(比如用户传了一堆普通文件,默认实现 `registered=false`)+ 没 arm timer,`will_sleep=false` 直接返 0——**没有东西能唤醒该任务,睡了就是死睡,不睡**。这条守护防住了「poll 一组永不阻塞的普通文件 + 无限超时」这种傻调用把任务挂死。

防 lost-wakeup 不是靠一个原子操作,是靠**「关中断 + fd 自己的锁」双保险**:

- **poll 侧**:`InterruptGuard` 把「Blocked 翻转 + 入队 + arm」关在一个 IRQ-off 窗口里。
- **fd 侧**:每个 fd 的 `poll_events` 实现在自己锁下既算 mask 又 `wait_enqueue`(pipe 是 `lock_.irq_guard()`、socket 是 `lock_.irq_guard()`)。

两层缺一不可——单核关中断够,但多核上别的 CPU 的 write 不受本地 `cli` 影响,必须靠 fd 自己的锁。这是 071 章立过的铁律,这里引用不重讲。

> **prepare_to_wait 契约的来历**:这套 `prepare_to_wait` / `schedule_blocked` / `wake_one` 模板在 071 章立过——那里把 pipe 阻塞从 `sti`/`hlt`(撞 `#DF`)改成真调度等待队列。poll 这里一字不动地复用同一套 proven 模板,只是把「挂一个队列」扩成「挂 N 个队列」。下一节讲这个扩展带来的新复杂度。
