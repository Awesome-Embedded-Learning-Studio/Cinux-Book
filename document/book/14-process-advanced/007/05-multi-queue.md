---
title: 05 · 一个 poll 睡在 N 个队列上 + 双唤醒源 + detach_all
---

# 一个 poll 睡在 N 个队列上 + 双唤醒源 + detach_all

这是 poll 相对单个 pipe read 的**全部新增复杂度**。pipe read 只睡在一个 `read_waiters_` 队列上;poll 把同一个 `self` 指针挂进每个被等 fd 的等待队列(`register_all` 循环)。后果有三个:

**后果一:任一 fd 的 wake_one 都能唤醒,但醒后不知道是谁叫的。** 任何一个 fd 的 producer(pipe write、socket on_data)调 `wake_one` 都能把 poll 唤醒。但 poll 醒来时拿不到「是 fd 3 触发了唤醒」这种来源信息——唤醒是单向的,`Scheduler::unblock` 只把 state 翻 Ready,不传「谁叫的」。所以醒来必须 `detach_all`(`poll_core.cpp:126-140`)遍历全部 fd 撤销注册,再 Pass 1 重扫由就绪位决定返回:

```cpp
// kernel/syscall/poll_core.cpp:126
void detach_all(kpollfd* pfds, uint64_t nfds, cinux::proc::Task* waiter) {
    for (uint64_t i = 0; i < nfds; ++i) {
        FdRef r = resolve_fd(pfds[i].fd);
        switch (r.kind) {
        case FdKind::kInode:
            r.inode->ops->poll_detach_waiter(r.inode, waiter);  // 从 fd 队列移除
            break;
        case FdKind::kConsole:
            cinux::drivers::console_tty().poll_detach(waiter);
            break;
        default:
            break;
        }
    }
}
```

这是「多路复用」落到 wait-queue 上的真样子:**不是 N 个 sleeper,是 1 个 sleeper 挂在 N 条链上**。poll_one 查就绪用的是同一个 self,但 self 被 register_all 挂进了 N 个队列;wake_one 是按队列叫的,叫醒的是这一个 self,但 self 不知道是从哪条链被叫的。

**后果二:stale waiter 必须撤干净。** poll task 醒来返回用户态后,task 结构迟早被释放/复用,但 N-1 个没叫醒它的 fd 队列里**还留着指向它的指针**。下一次这些 fd 有事件就会 `wake_one` 一个已经不存在的 task(`Scheduler::unblock` 对非 Blocked 是 no-op,看似无害),但更险的是 task 内存被复用后,这个 stale 指针指向了别人的 task——可能误把无关 task 翻成 Ready。所以返回前**不管谁叫醒的、不管超时还是就绪还是 EINTR**,一律 `detach_all` + `timer_queue_disarm`。三条返回路径全覆盖(就绪返回在 `poll_core.cpp:199`、超时/无唤醒源返 0 在 `:202`、EINTR 返 `-kEintr` 在 `:214`):

- 就绪返回(`became_ready` 分支):`detach_all` → `continue` 回 for 顶 Pass 1 报。
- 超时/无唤醒源返 0:这条是「sleep 之前就发现没东西能叫醒」直接返 0(`will_sleep=false`),不会进 schedule_blocked,detach 也就不需要(还没真挂出去)。
- EINTR 返 `-kEintr`:`detach_all`(`:205`)在 `schedule_blocked`(`:204`)之后调过,再检查信号(`:213-214`)。

**后果三:双唤醒源必须幂等。** 有限 timeout 的 poller 同时挂在「N 个 fd 队列」+「timer queue」上。任一 fd 来数据、或 timer 到点,都会调 `Scheduler::unblock`。如果 unblock 不是幂等的(把已 Ready 的 task 再次 enqueue),就会 double-add 进运行队列——同一个 task 在运行队列里出现两次,调度器抽到两次,栈就乱了。

`Scheduler::unblock`(`scheduler_block.cpp:48-70`)正是防这个——「state != Blocked 就 return」:

```cpp
// kernel/proc/scheduler_block.cpp:48
void Scheduler::unblock(lib::NotNull<Task*> task) {
    if (task == nullptr) {
        return;
    }
    // Idempotent (F4-M4 prepare-to-wait): only a still-Blocked task needs waking.
    // A task that is already Ready/Running was never put to sleep, or already won
    // a concurrent lost-wakeup race and is runnable -- re-enqueuing it would
    // double-add it to the run queue.  No-op in that case.
    if (task->state != TaskState::Blocked) {
        return;  // 已经 Ready/Running -> 不重复 enqueue
    }
    task->state = TaskState::Ready;
    // ... enqueue ...
}
```

这是「为什么敢同时挂两个唤醒源」的支点——fd 那侧叫了一次,timer 这侧又叫一次,后者是 no-op,不会 double-add。注释里那句 `Idempotent (F4-M4 prepare-to-wait)` 就是这个意思。

**EINTR 路径**:被信号叫醒的 poll,`do_poll_core` 在 `detach_all` 之后返 `-kEintr`(`poll_core.cpp:214`),POSIX poll 可中断。`schedule_blocked`(`scheduler_block.cpp:99-100`)的 TASK_INTERRUPTIBLE 检查——`wait_queue_head != nullptr` 且 `signal_deliverable_pending` 时翻回 Running 不真睡——是 071 章修的 busybox-ping `^C` 卡 Blocked 旧 bug。poll 这里复用同一检查,信号到了不真睡,直接走 EINTR 返回路径让信号处理跑。

`test_poll_write_wakes_registered_poller`(`test_poll.cpp:260-299`)是这一节的端到端证据——它 role-play 了「poller 挂在 pipe read 队列上、peer 写一字节、`wake_one` 把 poller 翻成 Ready」这条链路(测试 harness 是单线程的跑不了真阻塞循环,所以用 `NoRescheduleGuard` 手动驱动 commit 序列)。`poller->state == Ready` 这一断言就是 wake 命中的证据。
