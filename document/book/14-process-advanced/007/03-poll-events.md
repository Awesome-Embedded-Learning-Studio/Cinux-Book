---
title: 03 · 就绪缝:poll_events 二合一虚方法 + always_bits 透传
---

# 就绪缝:poll_events 二合一虚方法 + always_bits 透传

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
