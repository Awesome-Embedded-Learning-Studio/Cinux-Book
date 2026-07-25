---
title: 01 · 导引:点亮什么
---

# 导引:点亮什么

> 匿名 pipe 早就有(GUI 那卷的 031b 用过它跟子进程通信):`sys_pipe` 建一对 fd,一端写、一端读,字节流过管子。可它有两个硬伤一直留着:其一,读写**阻塞**是用 `sti; hlt` 自旋实现的——在 syscall 上下文里 `sti`,跟 059 那个 sys_ping #DF 是同一族要命的坑(LAPIC 时钟中断在那个 `sti` 窗口里抢 `%gs:0` 栈上的陷阱帧,sysretq 弹花就 #DF,真硬件必炸,harness 不跑 ring3 阻塞所以假绿盖着);其二,只能有亲缘关系的进程用(父子共享 fd)。这一章把这两件事都修了:阻塞改成真调度等待队列(复用 Mutex/console TTY 那套 proven 模板)、加 `O_NONBLOCK`、写已关读端触发 `SIGPIPE`,再立**命名 FIFO**——给 pipe 一个名字(挂进 `/dev`),任意进程按名字 open 两端,不用亲缘。
>
> A 档:punchline 是命名 FIFO 端到端走通:`mkfifo` 建个有名管道 → 一个进程 open 写端、另一个 open 读端 → 跨 fd 读写往返;还有 pipe 写已关读端真触发 SIGPIPE、O_NONBLOCK 满/空返 EAGAIN。这一章真正要讲的是「阻塞怎么做对」(sti/hlt 自旋为什么是坑、wait queue 怎么避开 lost wakeup)、以及「给匿名 pipe 套个名字」的 cloning open 模式(跟 066 PTY 的 `/dev/ptmx` 一个套路)。

## 这章咱们要点亮什么

1. **sti/hlt 自旋阻塞为什么是定时炸弹**:在 syscall 上下文 `sti`,时钟中断在那个窗口抢栈上陷阱帧 → sysretq 弹花 → #DF。修法是**真调度等待队列**,不是自旋。
2. **等待队列复用 proven 模板**:`prepare_to_wait` + `schedule_blocked` + `unblock`(Mutex、console TTY 阻塞读已验,lost-wakeup-safe 跨核)。
3. **O_NONBLOCK 不改 InodeOps 签名**:`Pipe::read/write` 加 `bool nonblock` 参数,满/空返哨兵 `PIPE_WOULDBLOCK`,ops 层映射 `WouldBlock`(-EAGAIN)。
4. **BrokenPipe / SIGPIPE**:写一个读端已关的 pipe → -EPIPE + SIGPIPE(靠 `reader_alive()` 区分「读端没了」和「参数错」)。
5. **命名 FIFO = 给 pipe 一个名字**:FifoRegistry 存「名字 → FIFO」,open 名字时 cloning 出 per-open 两端(首读者建共享 Pipe),跟 `/dev/ptmx` cloning 同套路。

下一节就按这三件(sti/hlt 坑与等待队列、BrokenPipe/SIGPIPE、命名 FIFO)展开实现。
