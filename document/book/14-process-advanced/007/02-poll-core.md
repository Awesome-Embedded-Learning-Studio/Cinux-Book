---
title: 02 · 多路复用这一层:poll_core 干什么
---

# 多路复用这一层:poll_core 干什么

先说为什么需要 poll。到 083 章为止,UnixSocket 的 recv、pipe 的 read、tty 的 read 都能真睡——字节没到,任务 Blocked;字节到了,producer 调 `wake_one` 把它叫醒。这套机制很美,但它有一个朴素约束:**一次阻塞调用只能等一个 fd**。

设想你在写一个 shell:它既要等用户敲键盘(stdin),又要等一个后台子进程通过 pipe 报状态。你只有一条主线程。傻办法是开两个线程各阻塞一个 fd,然后俩线程还得用别的机制把「谁先来」告诉你——这是把简单问题硬扭成并发问题。聪明的办法是**把 stdin 和 pipe 两个 fd 一次性塞进一个数组,告诉内核「这俩 fd 任一可读你就唤醒进程」**。内核替你盯着这俩 fd,谁先就绪就叫醒你,你醒后查 revents 看是谁。这就是 poll/select 这一层多路复用。

Cinux 把这件事收敛到**一个核心函数** `do_poll_core`(`poll_core.cpp:144`),poll 和 select 两个 syscall 都 funnel 进来:

```cpp
// kernel/syscall/poll_core.cpp:144
int64_t do_poll_core(kpollfd* pfds, uint64_t nfds, int64_t timeout_ms) {
    const bool     infinite = (timeout_ms < 0);
    const uint64_t deadline = (infinite || timeout_ms == 0)
                                  ? 0
                                  : monotonic_ns() + static_cast<uint64_t>(timeout_ms) * kNsPerMs;

    for (;;) {
        // Pass 1: readiness check, no registration (IRQs on).
        int64_t ready = 0;
        for (uint64_t i = 0; i < nfds; ++i) {
            pfds[i].revents = poll_one(pfds[i].fd, static_cast<uint16_t>(pfds[i].events));
            if (pfds[i].revents != 0) {
                ++ready;  // 累加 revents!=0 的 fd 计入返回值
            }
        }
        if (ready > 0) {
            return ready;  // 任一就绪:立刻返回就绪 fd 的个数
        }
        if (!infinite && monotonic_ns() >= deadline) {
            return 0;  // timed out (or timeout==0 single pass)
        }
        // ... 统一 park 块(下一节展开)...
    }
}
```

它的输入是一种内核内部表示 `kpollfd`(`poll_core.hpp:33-37`):

```cpp
// kernel/syscall/poll_core.hpp:33
struct kpollfd {
    int32_t fd;
    int16_t events;   // 用户想看的事件(POLLIN/POLLOUT/...)
    int16_t revents;  // 内核填的「实际就绪事件」
};  // 8 字节,刻意对齐 Linux x86-64 struct pollfd
```

这个 8 字节布局不是随便选的——它跟 Linux 用户态的 `struct pollfd` 一模一样,所以 `sys_poll` 能直接 `copy_from_user` / `copy_to_user` 整块搬,不用做任何字段重排。

主循环是个无界 `for(;;)`,每一轮做三段:

- **Pass 1 就绪扫描**:用 `poll_one` 只查就绪、**不注册** waiter(传 nullptr)。任一 fd 的 revents 非零,`ready` 累加。这里有个关键澄清——返回的是就绪 fd 的**个数**,不是第一个就绪 fd 的下标。多 fd 同时就绪会全部填好 revents 并一起计入返回值(`test_poll_two_fds_one_ready` 在 `test_poll.cpp:168-189` 验证:两个 fd 只有第二个就绪,返回 1,第一个 revents 保持 0)。
- **timeout 判定**:Pass 1 全没就绪,看是否超时。超时返 0。`timeout_ms < 0` 是无限等(不进这条),`timeout_ms == 0` 是单趟扫描(第一次进 for 顶 `deadline=0`、`monotonic_ns() >= 0` 永真,直接返 0)。
- **统一 park**:前两条都没命中,进 park 块真睡。这是下一节的主题。

`level-trigger` 这个语义要先讲清楚:每次回到 for 顶都**无条件重扫所有 fd 的当前状态**,只要此刻仍就绪就报 POLLIN,内核不记忆「这个 fd 上一轮报过没」。这跟 edge-trigger(epoll ET)完全不同——ET 只在状态翻转那一刻报一次,报完你得读完否则下次 poll 不再报。poll 是 LT,你 poll 一次报一次,只要数据还在就持续报。语义等同 Linux poll、等同 epoll LT。这一层选择 LT 不是偷懒,是 poll 这个 ABI 的本意——它没有「注册一次持续监控」的状态,每次调用都是独立的一次扫描。

阻塞真睡的模板——`prepare_to_wait` / `schedule_blocked` / `wake_one`——071 章立过:那里把 pipe 的阻塞 read 从 `sti`/`hlt`(会撞 `#DF`)改成真调度等待队列。poll 这里**一字不动地复用同一套 proven 模板**,不碰 `sti`/`hlt`。下一节讲 poll 怎么把这套模板扩成「一个任务睡在 N 个队列上」。

> **顺手提一句头注释的 stale**:`poll_core.hpp:17-20` 的注释大意是「有限 timeout 在 yield 自旋、真 timer-wake 是 F5-M4 DEBT」(原文见那几行)。但实现里 `poll_core.cpp:188` 早把 `timer_queue_arm` 调上了(`:207` 还有 `timer_queue_disarm` 配对),有限 timeout 也是真 park + timer-wake。这是文档落后于代码的典型,本章以 `.cpp` 实现为准。读者核源码时别被那段 header 注释带歪。
