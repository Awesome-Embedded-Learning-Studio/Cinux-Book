---
title: 084 · poll / select:同时等 N 个 fd,任一就绪即返回
---

# 084 · poll / select:同时等 N 个 fd,任一就绪即返回

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

## 多路复用这一层:poll_core 干什么

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

## 就绪缝:poll_events 二合一虚方法 + always_bits 透传

`do_poll_core` 要问每个 fd「你 ready 吗」,但每种 fd 的就绪判据完全不同:

- pipe 读端看 buffer 空不空、writer 还开不开;
- UnixSocket/TcpSocket 看 rx 环有没有数据、peer 关没关;
- console tty 看有没有 cooked 行;
- 普通文件**永远就绪**(read 不会阻塞)。

Cinux 的解法是给 `InodeOps` 加一个**二合一虚方法** `poll_events`(`inode.hpp:190-211` 头注释 + 签名):

```cpp
// kernel/fs/inode.hpp:190 (节选,头注释逐字)
/// poll/select readiness for this open file.
///
/// Returns the ready event mask (a subset of the @c kPoll* bits above).
/// sys_poll masks it against each pollfd's requested @c events to form
/// @c revents (POLLERR/POLLHUP/POLLNVAL are always passed through).
///
/// Wait registration: if @p waiter is non-null, ALSO enqueue it on this fd's
/// internal wait queue -- atomically with the readiness check, under this
/// fd's own lock (the prepare_to_wait contract).  A later state change
/// (bytes arrive / peer closes) then wakes it via Scheduler::unblock.  The
/// caller follows with poll_detach_waiter() once it no longer waits.
virtual uint32_t poll_events(const Inode* inode, cinux::proc::Task* waiter, bool* registered);
```

注意「二合一」这三个字——它不是单纯查询。一次调用同时干两件事:

1. 返回当前就绪掩码(POLLIN/POLLOUT/POLLHUP/POLLERR);
2. 若 `waiter` 非空,在持自己锁的前提下把它排进本 fd 的等待队列(就是被阻塞 recv/accept 睡的那个队列),通过 `registered=true` 出参告诉调用方「登记了,记得事后来 detach」。

为什么必须是二合一、不能拆成 `query_ready()` + `register_waiter()` 两个 API?因为「查就绪」和「入队」之间若有窗口,producer 恰好在窗口里写了再 `wake_one`(此时队列还是空,谁也没叫醒),然后你才挂上去睡——这个 wake 就永远丢了,你睡死。这就是 prepare_to_wait 契约的核心:两件事必须在 fd 自己的锁下**原子完成**。所以签名收敛成 `poll_events(inode, waiter, &registered)`,让 fd 实现在同一把锁里既算 mask 又 `wait_enqueue`。

事件位定义在 `inode.hpp:40-47`,跟 Linux UAPI 值对齐(本章用到的 POLLIN/POLLPRI/POLLOUT/POLLERR/POLLHUP/POLLNVAL 在 `:40-45`,另有 POLLRDNORM/POLLWRNORM 两位在 `:46-47`):

```cpp
// kernel/fs/inode.hpp:40
constexpr uint16_t kPollIn     = 0x0001;  ///< POLLIN  (readable: data available)
constexpr uint16_t kPollPri    = 0x0002;  ///< POLLPRI (priority / out-of-band data)
constexpr uint16_t kPollOut    = 0x0004;  ///< POLLOUT (writable: space available)
constexpr uint16_t kPollErr    = 0x0008;  ///< POLLERR (error condition; always reported)
constexpr uint16_t kPollHup    = 0x0010;  ///< POLLHUP (peer hung up; always reported)
constexpr uint16_t kPollNval   = 0x0020;  ///< POLLNVAL (invalid fd; always reported)
```

默认实现(`inode.cpp:105`)是「普通文件永远返回 POLLIN|POLLOUT、永不注册」:

```cpp
// kernel/fs/inode.cpp:105
uint32_t InodeOps::poll_events(const Inode*, cinux::proc::Task*, bool* registered) {
    if (registered != nullptr) {
        *registered = false;  // 普通文件不阻塞,从不注册 waiter
    }
    return kPollIn | kPollOut;  // 永远就绪:read/write 都不会阻塞
}
```

这个默认值是个**兼容性支点**:Cinux 已经有 ~20 个 `InodeOps` 子类(ext2 文件、设备节点、目录……),它们都没有阻塞语义。默认「永远就绪 + 永不注册」让这些子类**一行不改**就兼容 poll——poll 一个普通文件立刻返回 POLLIN|POLLOUT,符合 Linux 语义(普通文件的 read 不会阻塞,poll 自然永远就绪)。

只有**会阻塞**的 fd 类型才 override `poll_events`。拿 pipe 当典型(`pipe.cpp:375`,源码注释为英文,这里贴的是原文逻辑、注释为讲解便利译成中文):

```cpp
// kernel/ipc/pipe.cpp:375 (函数体逐字;行内注释源码为英文,这里译成中文方便读者)
uint32_t Pipe::poll_read_events([[maybe_unused]] cinux::proc::Task* waiter) {
    auto     g    = lock_.irq_guard();      // 进入 pipe 自己的锁 + IRQ off
    uint32_t mask = 0;
    // POLLIN whenever bytes are buffered; POLLHUP once the writer closes
    // (Linux reports both when unread data remains after close).
    if (!buf_.empty()) {
        mask |= cinux::fs::kPollIn;         // 有字节缓冲 -> 可读
    }
    if (!writer_open_) {
        mask |= cinux::fs::kPollHup;        // writer 关了 -> EOF(hangup)
    }
#ifndef CINUX_HOST_TEST
    // Register the poller ... Done under lock_ (and IRQs off) atomically with
    // the readiness check -- the prepare_to_wait contract.
    if (waiter != nullptr) {
        wait_enqueue(read_waiters_, waiter);  // 同一把锁下原子入队
    }
#endif
    return mask;
}
```

注意两个关键点:

- **mask 计算和 `wait_enqueue` 在同一把 `lock_.irq_guard()` 下**:这正是 prepare_to_wait 契约的「fd 这一侧」——查就绪和入队原子。pipe 的 `PipeReadOps::poll_events`(`pipe_ops.hpp:62` 注释明说「delegates to `Pipe::poll_read_events`」)就是把 `InodeOps::poll_events` 转发到这个方法。
- **POLLHUP 跟 POLLIN 不互斥**:`buf_` 里还有未读字节、但 writer 已经关了,mask 会同时置 POLLIN|POLLHUP——让你先把残留字节读完,再感知到 EOF。这是 Linux 的语义,`test_poll_writer_closed_pollhup`(`test_poll.cpp:238-250`)守住这条。

socket 那边的 override 是同一个虚方法——它们的就绪判据(rx 环、accept 队列、peer EOF)思路与 pipe 镜像,本章不展开实现细节,只点出它们 override 的就是 `InodeOps::poll_events`:

- `UnixSocket::poll_events`(`unix_socket.cpp:391`):listening 态看 accept 队列、connected 态看 rx 环 + POLLHUP(`peer_eof_` 在 `unix_socket.cpp:409`),waiter 排进 `accept_waiters_`/`recv_waiters_`。083 章讲过它的细节。
- `TcpSocket::poll_events`(`tcp_socket.cpp:291`):listening 态看 accept 队列、connected 态看 rx 环 + POLLHUP(`peer_closed_` 在 `tcp_socket.cpp:311`),waiter 排进 `accept_waiters_`/`recv_waiters_`。这个 override 跟 poll 一同落地(069 章只做 TCP 协议骨架,socket API 留到后续,没碰 poll_events),实现思路与 UnixSocket 镜像。
- `UdpSocket::poll_events`(`udp_socket.cpp`):同款。
- `SocketOps::poll_events`(`socket.cpp:129-138`)是 `InodeOps` 到 `Socket` 的委托桥——socket fd 的 Inode 是 `SocketOps`,它把 `poll_events` 转给绑定的 `Socket` 子类的 `poll_events`。

配套虚方法 `poll_detach_waiter`(`inode.hpp:218`,头注释从 `:213` 起)在 poller 醒来后撤销注册,默认 no-op(普通文件没注册过)。

现在看 `poll_one`——Pass 1 只查就绪、不注册的纯查询路径(`poll_core.cpp:77-92`):

```cpp
// kernel/syscall/poll_core.cpp:77
uint16_t poll_one(int fd, uint16_t events) {
    if (fd < 0) {
        return 0;  // negative fd: ignored, revents left at 0 (Linux)
    }
    FdRef    r      = resolve_fd(fd);
    uint32_t wanted = events | always_bits;  // 用户请求 + 三位无条件透传
    switch (r.kind) {
    case FdKind::kInode:
        return static_cast<uint16_t>(r.inode->ops->poll_events(r.inode, nullptr, nullptr) & wanted);
    case FdKind::kConsole:
        return static_cast<uint16_t>(cinux::drivers::console_tty().poll_events(nullptr, nullptr) &
                                     wanted);
    default:
        return cinux::fs::kPollNval;  // absent fd > 2 -> invalid
    }
}
```

这里的 `always_bits`(`poll_core.cpp:47`)是 POLLERR|POLLHUP|POLLNVAL——三位无条件透传,即使用户在 events 里没请求:

```cpp
// kernel/syscall/poll_core.cpp:47
constexpr uint16_t always_bits = cinux::fs::kPollErr | cinux::fs::kPollHup | cinux::fs::kPollNval;
```

这三位的来源各有故事:

- **POLLHUP**:writer 关了,读端感知到 EOF。pipe 是 `!writer_open_`(`pipe.cpp:383`),TcpSocket 是 `peer_closed_`(`tcp_socket.cpp:311`)、UnixSocket 是 `peer_eof_`(`unix_socket.cpp:409`)——两个 socket 类各自的字段名,语义相同(对端关)但名字不同。让你 poll 醒来 read 拿到 0(EOF)而不是永远阻塞。
- **POLLERR**:reader 关了,写端再写会 SIGPIPE。pipe 是 `!reader_open_`(`pipe.cpp:408`,写在 `poll_write_events` 里)。让你 poll 醒来 write 拿到错误。
- **POLLNVAL**:fd>2 且 fd 表里没条目——无效 fd。让你 poll 醒来知道这个 fd 压根不该等。

注意 fd≤2 且无表项是特例——`resolve_fd`(`poll_core.cpp:61-73`)把它派发到 `FdKind::kConsole`,走 legacy console TTY 兜底(早期启动 / 测试 stdin 没有 fd 表条目时,poll fd 0 还能查 console tty 的就绪):

```cpp
// kernel/syscall/poll_core.cpp:61
FdRef resolve_fd(int fd) {
    if (fd < 0) {
        return {FdKind::kNone, nullptr};
    }
    cinux::fs::File* file = cinux::fs::current_fd_table().get(fd);
    if (file != nullptr && file->inode != nullptr && file->inode->ops != nullptr) {
        return {FdKind::kInode, file->inode};  // 正常 fd 表条目
    }
    if (fd <= 2) {
        return {FdKind::kConsole, nullptr};  // legacy console stdin/stdout/stderr
    }
    return {FdKind::kNone, nullptr};  // fd>2 无条目 -> POLLNVAL
}
```

`poll_one` 传给 `poll_events` 的 waiter 是 `nullptr`——纯查询、不注册。register_all(下一节的 park 块)才会传真 waiter 进去注册。

## 统一 park:关中断 + register_all + schedule_blocked

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

## 一个 poll 睡在 N 个队列上 + 双唤醒源 + detach_all

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

## poll vs select:两个 ABI 共享 poll_core

poll 和 select 在 Cinux 里是「**两套 ABI、一个核心**」。核心 `do_poll_core` 不知 poll/select 为何物——签名只接 `kpollfd*` + nfds + timeout_ms(`poll_core.hpp:51`),内部没有任何「poll 模式 / select 模式」分支。两个 wrapper 的差别纯粹是「**数据形态转换**」。

### sys_poll:直接搬 pollfd 数组

`sys_poll`(`sys_poll.cpp:31`)是薄薄一层用户态边界 wrapper:

```cpp
// kernel/syscall/sys_poll.cpp:31
int64_t sys_poll(uint64_t fds_virt, uint64_t nfds, uint64_t timeout, uint64_t, uint64_t, uint64_t) {
    if (nfds > kPollMaxFds) {  // kPollMaxFds=64,栈上 pollfd 上限
        return -cinux::kEinval;
    }
    if (nfds == 0) {
        // poll(NULL, 0, timeout):可移植亚秒睡眠,无数组要拷
        return do_poll_core(nullptr, 0, static_cast<int64_t>(timeout));
    }
    if (fds_virt == 0) {
        return -cinux::kEfault;
    }

    kpollfd kfds[kPollMaxFds];  // 栈上 512B
    if (!cinux::user::copy_from_user(kfds, reinterpret_cast<void*>(fds_virt),
                                     nfds * sizeof(kpollfd))) {
        return -cinux::kEfault;
    }

    int64_t ready = do_poll_core(kfds, nfds, static_cast<int64_t>(timeout));

    if (!cinux::user::copy_to_user(reinterpret_cast<void*>(fds_virt), kfds,
                                   nfds * sizeof(kpollfd))) {
        return -cinux::kEfault;
    }
    return ready;
}
```

用户给的就是 `pollfd` 结构数组,wrapper 直接 `copy_from_user` 整块拷进来(nfds×8B),跑完 `copy_to_user` 整块拷回去。`pollfd` 是「紧凑结构数组」语义——每个 fd 自带想看什么事件(events)、就绪什么事件(revents)。栈上 cap `kPollMaxFds=64`(`sys_poll.cpp:29`)够真实 app 用(sh poll stdin、nc/wget poll 一个 socket),再大就 `-EINVAL` 而不是 heap alloc——hobby-OS 用「上限即真理」省栈空间。`poll(NULL, 0, timeout)` 是可移植亚秒睡眠的特殊用法(无数组要拷),`do_poll_core(nullptr, 0, timeout)` 直走核心。

### sys_select:fd_set 位图 ↔ pollfd 翻译

`sys_select`(`sys_select.cpp:69-191`)用户给的是 3 张位图(`fd_set`:readfds/writefds/exceptfds)+ nfds。wrapper 先扫描 fd_set 把被置位的 fd 合并成 pollfd 数组,交给 `do_poll_core`;跑完按返回的 revents 把就绪 fd 回填进 3 张输出位图。

**fd_set → pollfd 翻译**(`sys_select.cpp:114-137`):

```cpp
// kernel/syscall/sys_select.cpp:114
kpollfd  pfds[kPollMaxFds];
uint64_t count = 0;
for (uint64_t fd = 0; fd < nfds; ++fd) {
    uint16_t ev = 0;
    if (readfds != 0 && fd_is_set(rd, static_cast<int>(fd))) {
        ev |= cinux::fs::kPollIn;    // read 位置位 -> POLLIN
    }
    if (writefds != 0 && fd_is_set(wr, static_cast<int>(fd))) {
        ev |= cinux::fs::kPollOut;   // write 位置位 -> POLLOUT
    }
    if (exceptfds != 0 && fd_is_set(ex, static_cast<int>(fd))) {
        ev |= cinux::fs::kPollPri;   // except 位置位 -> POLLPRI
    }
    if (ev == 0) {
        continue;  // 这个 fd 三个集合都没置位 -> 跳过
    }
    if (count >= kPollMaxFds) {
        return -cinux::kEinval;  // too many watched fds for the stack cap
    }
    pfds[count].fd      = static_cast<int32_t>(fd);
    pfds[count].events  = static_cast<int16_t>(ev);
    pfds[count].revents = 0;
    ++count;
}

int64_t ready = do_poll_core(pfds, count, timeout_ms);
```

稀疏集合只生成被 watch 的 fd 项,不浪费 pollfd 槽。

**revents → fd_set 回填**(`sys_select.cpp:144-165`):

```cpp
// kernel/syscall/sys_select.cpp:144
// Rebuild the output sets IN PLACE (zero, then set ready bits).  POLLHUP/
// POLLERR on a watched read fd surface in the read set so the app wakes and
// reads EOF / gets the error (Linux reports them this way).
for (uint64_t i = 0; i < kSetBytes; ++i) {
    rd[i] = 0; wr[i] = 0; ex[i] = 0;  // 先清零输出集合
}
for (uint64_t i = 0; i < count; ++i) {
    uint16_t rv = static_cast<uint16_t>(pfds[i].revents);
    int      fd = pfds[i].fd;
    if (rv == 0) {
        continue;
    }
    if (readfds != 0 &&
        (rv & (cinux::fs::kPollIn | cinux::fs::kPollHup | cinux::fs::kPollErr))) {
        fd_set_bit(rd, fd);  // POLLIN/HUP/ERR 都进 read set(Linux 反直觉语义)
    }
    if (writefds != 0 && (rv & cinux::fs::kPollOut)) {
        fd_set_bit(wr, fd);
    }
    if (exceptfds != 0 && (rv & cinux::fs::kPollPri)) {
        fd_set_bit(ex, fd);
    }
}
```

注意一个反直觉但符合 Linux 的取巧——**POLLHUP/POLLERR 也进 read set**(`sys_select.cpp:155-157`)。一个写端关闭的 pipe,poll 在读 fd 报 POLLHUP,select 会把这个 fd 在 readfds 置位,让 app 醒来 read 拿 EOF 或错误,而不是永远阻塞。这条语义的依据是 Linux man select(2):「a file descriptor that has reached end-of-file will be reported as ready for reading」。

### 两个有意思的取巧

**取巧一:只拷 fd_set 低 32B。** `kSetBytes = FD_TABLE_SIZE / 8 = 32B`(`sys_select.cpp:41`),即 256 位。用户态 `fd_set` 是 `FD_SETSIZE=1024` 位 = 128B,但 Cinux 一个进程最多持 `FD_TABLE_SIZE=256` 个 fd(`file.hpp`),bits 256..1023 永远不可能就绪,所以只拷 32B,nfds 上界 clamp 到 256(`sys_select.cpp:72-74`)。这是 hobby-OS 用「上限即真理」省栈空间的典型——栈上工作集从 384B(三张 128B 位图)压到 96B(三张 32B)。

**取巧二:*timeout 回填剩余时间。** Linux 的 select 会把 `*timeout` 更新为「剩余时间」(从不增加),Cinux 照搬(`sys_select.cpp:180-188`):

```cpp
// kernel/syscall/sys_select.cpp:180
if (has_deadline) {
    uint64_t now    = monotonic_ns();
    int64_t  rem_ns = (now >= deadline) ? 0 : static_cast<int64_t>(deadline - now);
    ktimeval tv;
    tv.tv_sec  = rem_ns / static_cast<int64_t>(kNsPerSec);
    tv.tv_usec = (rem_ns % static_cast<int64_t>(kNsPerSec)) / 1000;
    static_cast<void>(cinux::user::copy_to_user(reinterpret_cast<void*>(timeout_virt), &tv, sizeof(tv)));
}
```

这条语义让 app 能用 select 实现「在 N 个 fd 上分时片轮转」——每次 poll 后看剩余时间决定下次等多久。

### SYS_poll=7 是「换芯」而非新增

`SYS_poll = 7`(`syscall_nums.hpp:31`)很早就占了号——`syscall_nums.hpp:31` 的注释明说 `(stub: busybox sh)`,当年那是个 stub,让 busybox sh 能起来。后来才把 handler 换成真阻塞 `do_poll_core`。所以 `syscall.cpp:170` 的源码注释是 `F8-M5 real poll` 而非 `add poll`。

对比 `SYS_select = 23`(`syscall_nums.hpp:44`,注释 `select (F8-M5 real poll/select)`)是同时新加的号。注册在 `syscall.cpp:170-171`:

```cpp
// kernel/arch/x86_64/syscall.cpp:170
syscall_register(SyscallNr::SYS_poll, sys_poll);            // F8-M5 real poll
syscall_register(SyscallNr::SYS_select, sys_select);        // F8-M5 real select
```

把「号早就占了、内核却很晚才会真等」的演进讲清楚,否则学习者会困惑「为什么 poll 是低号(7)但实现晚」——答案是早期那个号是个 stub(具体返回什么已不可考,工作树里只有换成真阻塞之后的实现),后来才把芯换成真阻塞 `do_poll_core`。这跟 SYS_select 这种「号和实现同时落地」的路径不一样。

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