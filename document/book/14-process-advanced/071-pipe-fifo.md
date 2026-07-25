---
title: 071 · Pipe 增强与命名 FIFO:阻塞、SIGPIPE、给管道起个名字
---

# 071 · Pipe 增强与命名 FIFO:阻塞、SIGPIPE、给管道起个名字

> 匿名 pipe 早就有(GUI 那卷的 031b 用过它跟子进程通信):`sys_pipe` 建一对 fd,一端写、一端读,字节流过管子。可它有两个硬伤一直留着:其一,读写**阻塞**是用 `sti; hlt` 自旋实现的——在 syscall 上下文里 `sti`,跟 059 那个 sys_ping #DF 是同一族要命的坑(LAPIC 时钟中断在那个 `sti` 窗口里抢 `%gs:0` 栈上的陷阱帧,sysretq 弹花就 #DF,真硬件必炸,harness 不跑 ring3 阻塞所以假绿盖着);其二,只能有亲缘关系的进程用(父子共享 fd)。这一章把这两件事都修了:阻塞改成真调度等待队列(复用 Mutex/console TTY 那套 proven 模板)、加 `O_NONBLOCK`、写已关读端触发 `SIGPIPE`,再立**命名 FIFO**——给 pipe 一个名字(挂进 `/dev`),任意进程按名字 open 两端,不用亲缘。
>
> A 档:punchline 是命名 FIFO 端到端走通:`mkfifo` 建个有名管道 → 一个进程 open 写端、另一个 open 读端 → 跨 fd 读写往返;还有 pipe 写已关读端真触发 SIGPIPE、O_NONBLOCK 满/空返 EAGAIN。这一章真正要讲的是「阻塞怎么做对」(sti/hlt 自旋为什么是坑、wait queue 怎么避开 lost wakeup)、以及「给匿名 pipe 套个名字」的 cloning open 模式(跟 066 PTY 的 `/dev/ptmx` 一个套路)。

## 这章咱们要点亮什么

1. **sti/hlt 自旋阻塞为什么是定时炸弹**:在 syscall 上下文 `sti`,时钟中断在那个窗口抢栈上陷阱帧 → sysretq 弹花 → #DF。修法是**真调度等待队列**,不是自旋。
2. **等待队列复用 proven 模板**:`prepare_to_wait` + `schedule_blocked` + `unblock`(Mutex、console TTY 阻塞读已验,lost-wakeup-safe 跨核)。
3. **O_NONBLOCK 不改 InodeOps 签名**:`Pipe::read/write` 加 `bool nonblock` 参数,满/空返哨兵 `PIPE_WOULDBLOCK`,ops 层映射 `WouldBlock`(-EAGAIN)。
4. **BrokenPipe / SIGPIPE**:写一个读端已关的 pipe → -EPIPE + SIGPIPE(靠 `reader_alive()` 区分「读端没了」和「参数错」)。
5. **命名 FIFO = 给 pipe 一个名字**:FifoRegistry 存「名字 → FIFO」,open 名字时 cloning 出 per-open 两端(首读者建共享 Pipe),跟 `/dev/ptmx` cloning 同套路。

## 第一个硬伤:sti/hlt 自旋阻塞是定时炸弹

匿名 pipe 的读写,写满/读空时该阻塞。原来的实现是自旋:`irq_enable(); for(一百万次){ hlt(); irq_disable(); 探一下有没有数据; irq_enable(); }`。这在 syscall 上下文里跑(`sys_write → do_write_kernel → pipe->write`),问题在于那个 `sti`(irq_enable)——

> 跟 059(sys_ping #DF)是同一个坑,记一笔串起来。syscall 进来时,陷阱帧压在内核栈上(由 `%gs:0` 指向)。`sti` 打开一个窗口,这个窗口里 LAPIC 时钟中断来了——中断处理要压自己的帧,可能踩到/挪动那个 syscall 陷阱帧的位置,等 syscall 用 sysretq 弹帧返回时,弹出来的花是错的 → #DF(Double Fault),真硬件必炸。harness 不真跑 ring3 的阻塞路径(测试都是内核态直调),所以 931/0 假绿一直盖着这个隐患。059 的 sys_ping 是这么炸的,pipe 阻塞也是这么炸的——同根。

修法是**真调度等待队列**,不自旋。复用现成 proven 模板:`prepare_to_wait()` + `schedule_blocked()` + `unblock()` + `Task::wait_next` 内侵式等待队列——这套 Mutex(`kernel/proc/sync.cpp`)和 console TTY 阻塞读(062)已经验过,lost-wakeup-safe 跨核。Pipe 加两条队列 `read_waiters_` / `write_waiters_`:

- 写满 / 读空:在 `lock_.irq_guard()` 下把自己 enqueue 进等待队列 + `prepare_to_wait`(「检查 + 登记 + 标 Blocked」关中断下原子,跟 062 console TTY 一个铁律,防 lost wakeup),**放锁放中断** → `schedule_blocked()` 切走;对端排空/写入后 `wake_one` 把我唤醒。
- close 读端 → `wake_all(write_waiters_)`(被唤醒的 writer 重试,见 `reader_alive()` false → BrokenPipe/SIGPIPE);close 写端 → `wake_all(read_waiters_)`(reader 见 EOF)。

`wake_one` 在持锁下调 `unblock` 是安全的:pipe 不是互斥锁(不交接所有权,被唤醒任务之后重新获取锁;scheduler run-queue 锁绝不跨 pipe 锁获取 → 无 AB-BA 死锁)。

## BrokenPipe / SIGPIPE + O_NONBLOCK

阻塞修好了,再加两个语义。

**BrokenPipe / SIGPIPE。** 往一个读端已经关了的 pipe 写,POSIX 规定返 -EPIPE 并给写进程发 SIGPIPE。Pipe 的 write 返回值要能区分「读端没了」(该 BrokenPipe)和「参数错」(该 InvalidArgument):靠 `reader_alive()`——write 返负数时,`reader_alive()` false 就是 BrokenPipe,否则 InvalidArgument。ops 层(`pipe_ops.cpp`)把 BrokenPipe 映射成 `Error::BrokenPipe`,sys_write 见到它返 -EPIPE 并 raise SIGPIPE。

**O_NONBLOCK。** 有时候不想阻塞(满/空就立刻返 EAGAIN,程序自己去干别的)。`Pipe::read/write` 加个 `bool nonblock` 参数(默认 false,旧的 2 参数调用不破):满/空且 nonblock 时返哨兵 `PIPE_WOULDBLOCK(-2)`,ops 层映射 `Error::WouldBlock`(-EAGAIN)。

> 这两件都**不改 `InodeOps::read/write` 的虚函数签名**——那玩意 blast radius 大(PTY 066 刚动过,ext2、DevFS 都实现它)。nonblock 标志收进 `PipeReadOps/PipeWriteOps` 的成员(构造时传),`InodeOps::read/write` 签名一行不动。匿名 pipe 的两端始终 blocking(sys_pipe 建的);O_NONBLOCK 经 FIFO 的 open 落地(下面)。

## 第二个硬伤:给 pipe 起个名字(命名 FIFO)

匿名 pipe 只能亲缘用(父子共享 fd)——因为两端 fd 是 `sys_pipe` 一次建出来的,得靠 fork 继承传给两个进程。可很多场景是两个**没亲缘关系**的进程想通信:它们怎么找到同一根管子?靠**名字**。命名 FIFO 就是「挂个名字的 pipe」:在 `/dev` 下建个有名节点,任意进程按名字 open 读端、另一个 open 写端,两端 open 齐了就接通同一根 Pipe。

这套靠三件:

**FifoRegistry。** 一个内存表「名字 → FIFO 条目」(`fifo.hpp:96`)。`mkfifo(name)` 在表里建条目(`sys_mknod` 只接 `S_IFIFO`,别的节点类型返 -ENOSYS);条目里的 Pipe 是**懒建**的——open 时才造。

**Cloning open(首读者建 Pipe)。** 这跟 066 PTY 的 `/dev/ptmx` 一个套路:open 一个 FIFO 名字,不是返回那个 inode 本身,而是 **clone 出一个 per-open 的新 Pipe 端**。第一次有人 open 读端时,建一根新的共享 Pipe,记在 FIFO 条目里;之后 open 写端(或更多读端)clone 出接同一根 Pipe 的端。所以「同一个名字」接的是「同一根 Pipe」,而每个 open 拿到的是自己的 per-open 端(独立 fd)。open 级的两端就绪阻塞(读写端都 open 了才接通)这一章推迟了,只做数据级阻塞。

**DevFS 节点 + sys_mknod。** FIFO 名字挂在 `/dev`(DevFS 的 dynamic_lookup 回调先查 FIFO 再落 PTY,跟 066 一样的 dynamic lookup 缝)。`sys_mknod(path, S_IFIFO|mode, 0)` 是系统调用入口(`mkfifo` 是它的 libc 拼写),走 061 那套 `do_mknod_kernel` / `sys_mknod` 分层。

shell 里加了 `mkfifo` 命令(建 FIFO)和 `fifotest` 命令(端到端验:mkfifo → open 写端写 → open 读端读 → 核对内容),用户态真闭环验证。

## 验证

四层。

**第一层:host 单测。** `test/unit/test_fifo.cpp`(FIFO cloning 逻辑、registry)+ `test/unit/test_sys_pipe.cpp` / pipe 单测(nonblock 负测:满 pipe nonblock 写 → PIPE_WOULDBLOCK、空 pipe nonblock 读 → PIPE_WOULDBLOCK、writer 关后 → 0 EOF、ops 映射 WouldBlock)。

**第二层:kernel 测试,端到端。** `kernel/test/test_fifo.cpp` 三例,走真路径(`do_mknod_kernel`/`do_write_kernel`/`do_read_kernel`):mkfifo → FifoRegistry lookup → FifoOps cloning 出写端+读端 → 跨 fd write/read round-trip 内容核对;mknod 非 FIFO → -ENOSYS;重复 mkfifo → -EEXIST。`test_pipe.cpp`/`test_sys_pipe.cpp` 守 SIGPIPE + nonblock。

**第三层:shell 真闭环。** `mkfifo` + `fifotest` 命令在 shell 里跑,用户态端到端验 FIFO。

**第四层:全量。** `run-kernel-test-all` 两腿各 1019 passed / 0 failed(基线 + SIGPIPE 1 + nonblock 3 + FIFO 3),0 panic / 0 #DF。

> 真阻塞唤醒的正确性靠「对齐 Mutex/console_tty 的 proven 模板 + 代码审查」——harness 单线程没法确定性测两个任务阻塞唤醒,所以负测守住 nonblock 语义 + 旧 pipe 测零回归,真阻塞靠模板复用的信心(同一套 wait queue,Mutex/TTY 已验过跨核 lost-wakeup-safe)。

## 这章没做的

- **open 级两端就绪阻塞**:数据级阻塞(读空/写满)做了;「读写端都 open 了才接通」的 open 级阻塞推迟(本里程碑只做数据级保正确)。
- **close 不销毁 pipe**:close 一个端不拆掉共享 Pipe 开新 epoch——`InodeOps::release` 虚槽已经有了(`inode.hpp:228`),匿名 pipe 两端 ops 的 `release` 也实现了(`pipe_ops.cpp:73/126` 做 DEBT-023 引用计数,见 `pipe.hpp:124-135`);但 **FIFO cloning 出的 per-open end inode 没人 free**——`FifoOps` 没覆写 `release`,所以 open cloning 时 `new` 出来的 per-open inode + 它的 PipeReadOps/PipeWriteOps 在 close 时仍泄漏(`fifo.cpp:156` 注释仍写「intentional leak on close」)。同匿名 pipe 的 hobby-OS 限制。
- **FIFO flat 命名空间**:都落 `/dev` flat,不在任意路径。任意路径 mkfifo + 用户态 shell 真闭环要 `do_openat_kernel` 也调 open cloning,留后续。
- **ConditionVariable 抽象**:这一章复用现成 wait queue,没抽独立的 ConditionVariable(留 sync 里程碑)。

## 小结

- 匿名 pipe 两个硬伤:阻塞用 sti/hlt 自旋(syscall 里 sti → 时钟中断抢栈陷阱帧 → #DF,跟 059 sys_ping 同根,harness 假绿盖着);只能亲缘用。
- 阻塞改真调度等待队列(`prepare_to_wait`+`schedule_blocked`+`unblock`,复用 Mutex/console_tty proven 模板,lost-wakeup-safe);Pipe 加 read/write_waiters_。
- BrokenPipe/SIGPIPE(write 已关读端 → -EPIPE + SIGPIPE,靠 reader_alive 区分)+ O_NONBLOCK(满/空返 PIPE_WOULDBLOCK → -EAGAIN,不改 InodeOps 签名,nonblock 收 ops 成员)。
- 命名 FIFO = 给 pipe 起名:FifoRegistry(名字→FIFO)+ cloning open(首读者建共享 Pipe,per-open 端,跟 /dev/ptmx 同套路)+ sys_mknod/mkfifo(S_IFIFO)+ DevFS 节点。shell mkfifo/fifotest 端到端。
- 诚实边界:open 级两端就绪阻塞推迟、close 不销毁 pipe、FIFO flat /dev、ConditionVariable 留 sync 里程碑。
