---
title: 07 · 验证、没做的、小结
---

# 验证、没做的、小结

## 验证:9 个 kernel 测试端到端跑通

`test_poll.cpp` 里有 9 个用例,在 `run_poll_tests`(`test_poll.cpp:303-317`)注册成 `Poll / Select Tests (028e)` section。逐类看:

**就绪语义(5 例,timeout=0 立即返回):**

- `test_poll_data_ready_pollin`(`:131-141`):pipe 写「hi」,poll 读端 → 返回 1,revents=POLLIN。
- `test_poll_empty_returns_zero`(`:144-153`):空 pipe(writer 还开),poll → 返回 0,revents=0。
- `test_poll_write_end_pollobut`(`:156-165`):非满 pipe 写端,poll → 返回 1,revents=POLLOUT。
- `test_poll_two_fds_one_ready`(`:168-189`):两个 pipe,只有第二个有数据,poll 两个 → 返回 1,第一个 revents=0、第二个 revents=POLLIN。这是「多 fd 同时等、只就绪的计返回值」的最小证据。
- `test_poll_writer_closed_pollhup`(`:238-250`):`pipe->close_writer()` 后,`PipeReadOps::poll_events` 报 POLLHUP、waiter=null 时 registered=false。

**有限超时真 park + timer-wake(1 例):**

- `test_poll_finite_timeout_parks_then_returns_zero`(`:194-204`):空 pipe,poll 读端 timeout=30ms。这会真进 park 路径——`register_all` 挂进 read 队列、`timer_queue_arm` 挂 30ms deadline、`schedule_blocked` 切走。30ms 后 timer tick 调 `unblock`,返回 0,revents=0。这条用例是有限 timeout **真 park + timer-wake** 的端到端证据(不是 yield 自旋,尽管头注释还写 DEBT)。

**事件唤醒 role-play(1 例):**

- `test_poll_write_wakes_registered_poller`(`:260-299`):`NoRescheduleGuard` 下手动驱动——`prepare_to_wait(poller)` 翻 Blocked、`poll_events(ri, poller, &registered)` 注册到 read 队列(registered=true、mask=0 因为空)、然后 peer write 一字节触发 `wake_one` → `unblock`、断言 `poller->state == Ready`、再查 `poll_events` 报 POLLIN。这是「producer wake 把挂在 read 队列上的 poller 叫醒」整条链路的手动重放。

**负路径(2 例):**

- `test_poll_negative_fd_ignored`(`:207-214`):fd=-1,revents 必须重置为 0(初始 0x55 sentinel 被清零),返回 0。Linux 语义:负 fd 被忽略。
- `test_poll_closed_fd_pollnval`(`:217-232`):fd>2 且 fd 表无条目 → revents=POLLNVAL,返回 1。

这 9 例覆盖了就绪掩码计算、level-trigger 多 fd 同时等、有限 timeout 真 park + timer-wake、事件唤醒 role-play、always_bits 透传(POLLHUP/POLLNVAL)五大路径。`do_poll_core` 在 Book 工作树上**真能端到端跑**——多 fd 同时等 + 真阻塞 wait-queue + 有限超时 timer-wake,全部有测试守住。

## 这章没做的

诚实的边界清单:

1. **`prepare_to_wait`/`schedule_blocked`/`unblock` 的 Scheduler 内部状态机**:那是 071 章 proven 模板,本章只引用「抄同款模板」。`wait_queue.hpp` 的 `wait_enqueue`/`wait_remove`/`wake_one`/`wake_all` 原语实现也是 071 章讲过的,本章不展开。
2. **各 socket 的 `poll_events` 就绪判据细节**:UnixSocket 怎么判 accept 队列、rx 环、peer EOF——083 章对象。TcpSocket/UDP socket 的 `poll_events` 是跟 poll 一同落地的 override(069 章只做 TCP 协议骨架,socket API 留到后续,没碰 poll_events),实现思路与 UnixSocket 镜像,本章只指出它们 override 的是同一个 `InodeOps::poll_events` 虚方法。
3. **`dup` 后 last-close 的 POLLHUP 时序**:测试 `test_poll_writer_closed_pollhup` 用的是 `Pipe::close_writer()` 直接调用(隔离测 `poll_events` 的 mask 计算,不掺 fd-close 路径的复杂性)。DEBT-023 的 last-close release hook 其实已经接上了——`PipeReadOps::release` / `PipeWriteOps::release`(`pipe_ops.hpp:70` / `:117`)在最后一个引用归零时调 `Pipe::release_read_ref` / `release_write_ref`(`pipe.cpp:315` / `:333`),翻转 `reader_open_`/`writer_open_` 并 `wake_all` 对端,从而触发 POLLHUP/POLLERR。真正还要 last-close 语义的边界点是:`dup` 一个 pipe fd 让 refcount>1 后,单个 close 不会立即 POLLHUP,要等最后一个引用归零——这条时序本章不展开。
4. **timer_queue 的 tick 扫描与 SMP 安全细节**:F5-M4 follow-up 的另一议题,本章只确认 `timer_queue_arm`/`disarm` 接口存在且 `poll_core.cpp:188`/`:207` 调用,不展开内部实现。
5. **`ppoll`/`pselect`(atom 版带信号掩码变体)**:经 grep 确认 Cinux 全无(`SYS_ppoll`/`SYS_pselect6` 不存在)。Linux 的 `ppoll`/`pselect6` 在 poll/select 基础上原子地换信号掩码,避免「poll 之前信号到了、handler 跑完、poll 又阻塞」的竞态——Cinux 这会儿没做,如实说「未实现」。
6. **header 注释 stale**:`poll_core.hpp:17-20` 还写「有限 timeout 在 yield 自旋、真 timer-wake 是 F5-M4 DEBT」——实现里早补上了(`poll_core.cpp:188` arm + `:207` disarm)。本章以 `.cpp` 为准,读者核源码时别被那段 header 带歪(顺手修掉是另一回事,本章不替你改源码)。

## 小结

poll/select 是 Cinux 把「一个任务等一个 fd」扩成「一个任务同时等 N 个 fd」的那一层。整套机制收敛到一个核心函数 `do_poll_core`,poll 和 select 两个 syscall 都 funnel 进来,差别纯粹是用户态数据形态——poll 给 pollfd 数组、select 给 fd_set 位图,核心不知 poll/select 为何物。四件工程决定撑起这一章:

- **多路复用这一层**:`do_poll_core` 是个无界 `for(;;)`,每轮 Pass 1 只查就绪不注册、任一就绪立刻返回就绪 fd 的个数、否则进统一 park。level-trigger——每次回到 for 顶无条件重扫,语义等同 Linux poll、等同 epoll LT。
- **poll_events 二合一虚方法**:`InodeOps::poll_events(inode, waiter, &registered)` 一次调用同时返回就绪掩码 + 把 waiter 排进 fd 的等待队列。两件事必须在 fd 自己的锁下原子完成,否则丢唤醒。默认「普通文件永远就绪、永不注册」让 ~20 个已有子类零改动兼容。
- **统一 park 防丢唤醒**:`InterruptGuard` 关 IRQ + `prepare_to_wait` 翻 Blocked + `register_all` 挂 N 个队列 + `timer_queue_arm` 挂 deadline,四步在一个 IRQ-off 窗口里原子完成。`became_ready` 二次确认就绪防「查完没就绪 → 准备睡 → 数据到了 → 你却睡了」;`will_sleep=false` 守护防「infinite 且无唤醒源」挂死。
- **一个 poller 睡在 N 个队列上**:同一个 `self` 被 `register_all` 挂进每个被等 fd 的队列,任一 fd 的 `wake_one` 都能叫醒,但醒后不知道是谁叫的——所以 `detach_all` 遍历全部 fd 撤销注册,防 stale waiter 悬挂。`Scheduler::unblock` 的幂等(state != Blocked 就 no-op)是「敢同时挂 fd + timer 两个唤醒源」的支点。

阻塞那套 `prepare_to_wait`/`schedule_blocked`/`wake_one` 模板是 071 章修 pipe 阻塞时立的 proven 模板,poll 这里一字不动地复用,只是把「挂一个队列」扩成「挂 N 个队列」。UnixSocket 的 `poll_events` 实现是 083 章讲过的,TcpSocket/UDP 的 `poll_events` 是跟 poll 一同落地的 override——本章只引用。`SYS_poll=7` 是「换芯」(早期 stub、后来换成真阻塞),`SYS_select=23` 是同时新加的号。9 个 kernel 测试端到端跑通就绪语义、有限 timeout 真 park + timer-wake、事件唤醒 role-play、POLLHUP/POLLNVAL 透传——poll/select 在 Book 既能多 fd 同时等,又能真阻塞 wait-queue,可用。
