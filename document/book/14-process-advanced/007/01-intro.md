---
title: 01 · 导引:点亮什么
---

# 导引:点亮什么

> 到 083 章为止,UnixSocket、pipe、tty 各自的阻塞 read/recv 都能真睡——`prepare_to_wait` 把 state 翻 Blocked,字节到了 `wake_one` 把任务叫醒。但这套「一睡一叫」有个硬伤:**一个任务只能睡在一个 fd 的等待队列上**。你想同时等键盘有输入、又等一个 socket 来数据,谁先到处理谁——傻办法是开两个任务各阻塞一个 fd,然后俩任务之间还得自己同步;聪明的办法是**一次把 N 个 fd 交给内核,任一就绪(或超时)就让内核叫醒你**。这就是 poll/select 这一层多路复用要干的事。
>
> 这一章的真主题不是「poll 的位怎么填」——那是用户态手册的活。真正要讲清的是四件落地的工程决定:其一,多路复用这一层只新增一个核心函数 `do_poll_core`(`poll_core.cpp:144`),它做的事是**收集各 fd 的就绪掩码 + 真阻塞等到任一就绪**,替换「每个 fd 各自阻塞读一次」的傻办法;其二,poll 的「询问就绪」和「把自己挂进 fd 的等待队列」不是两个 API,是**同一个虚方法 `poll_events` 的二合一**——一次调用同时返回掩码和注册 waiter;其三,poll 和 select 是**两套 ABI、一个核心**——`sys_poll` 直接搬 pollfd 数组,`sys_select` 把 fd_set 位图翻成 pollfd 再喂同一个 `do_poll_core`;其四,一个 poll 任务**同时睡在 N 个队列上**——任一 fd 的 `wake_one` 都能叫醒它,醒后必须 `detach_all` 遍历全部 fd 撤销注册,防 stale waiter 悬挂。阻塞的那套 `prepare_to_wait`/`schedule_blocked`/`wake_one` 模板不是新东西——071 章修 pipe 阻塞时立的,这里一字不动地复用;UnixSocket 那边的 `poll_events` 实现也是 083 章讲过的,本章只引用「它们 override 的就是同一个虚方法」。

## 这章咱们要点亮什么

1. **多路复用这一层干什么**:`do_poll_core`(`poll_core.cpp:144`)是个无界 `for(;;)`,每一轮做三段——Pass 1 用 `poll_one` 只查就绪不注册、任一就绪立刻返回就绪 fd 的个数、否则进统一 park。返回值是就绪 fd 的**个数**(累加 `revents != 0` 的 pfd),不是第一个就绪 fd——多 fd 同时就绪会全部填好 revents 并计入返回值(`test_poll.cpp:168-189` 验证)。
2. **level-trigger,不是 edge**:每次回到 for 顶都无条件重扫所有 fd 当前状态,只要此刻仍就绪就报 POLLIN,不记忆「是否已报告过」。语义等同 Linux poll、等同 epoll LT。
3. **poll_events 是二合一虚方法**:`InodeOps::poll_events(inode, waiter, &registered)`(`inode.hpp:211`)一次调用同时干两件事——返回当前就绪掩码、若 waiter 非空把它排进本 fd 的等待队列。默认实现是「普通文件永远返回 POLLIN|POLLOUT、永不注册」,让 ~20 个已有子类零改动兼容。
4. **always_bits 透传**:POLLERR/POLLHUP/POLLNVAL 三位无条件透传,即使用户没在 events 里请求。writer 关了 → 读端 POLLHUP、reader 关了 → 写端 POLLERR、fd>2 且无表项 → POLLNVAL。
5. **统一 park 防丢唤醒**:`prepare_to_wait` 翻 Blocked + `register_all` 把同一个 waiter 挂进每个可阻塞 fd 的队列 + `timer_queue_arm` 挂一个绝对 deadline,这三步必须在一个关中断(InterruptGuard)窗口里原子完成——否则经典 lost-wakeup:你查完没就绪还没挂上队列,producer 写一个字节 `wake_one` 找到空队列,然后你才挂上去睡死。
6. **一个 poller 睡在 N 个队列上**:`register_all` 把同一个 `self` 指针挂进每个被等 fd 的等待队列。任一 fd 的 producer `wake_one` 都能叫醒 poller,但醒后不知道是谁叫的,所以必须 `detach_all` 遍历全部 fd 撤销注册,再 Pass 1 重扫由就绪位决定返回。多路复用落到 wait-queue 上不是 N 个 sleeper,是 1 个 sleeper 挂在 N 条链上。
7. **poll vs select 共享核心**:`do_poll_core` 不知 poll/select 为何物,签名只接 `kpollfd*` + nfds + timeout_ms。`sys_poll` 直接 `copy_from_user` pollfd 数组;`sys_select` 把 fd_set 位图翻译成 pollfd[] 再喂核心,跑完把 revents 反翻译回位图。两套 ABI 行为永远一致——这正是 Linux 自己的思路(Linux 内核 select 也内部转成 pollfd 走同一 `do_sys_poll`)。

接下来六节按这七点的顺序逐一落地:poll_core 主循环、poll_events 二合一、统一 park、N 队列多路复用、poll/select 两套 ABI,最后验证、诚实边界与小结。
