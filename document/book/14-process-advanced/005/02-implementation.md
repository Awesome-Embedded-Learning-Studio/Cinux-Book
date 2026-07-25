---
title: 02 · 阻塞、SIGPIPE、命名 FIFO 三件实现
---

# 阻塞、SIGPIPE、命名 FIFO 三件实现

这一节把导引里点亮的五件,按「第一个硬伤(sti/hlt)→ BrokenPipe/SIGPIPE + O_NONBLOCK → 第二个硬伤(命名 FIFO)」的顺序逐一落地。

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

三件齐了,下一节进验证、诚实边界与小结。
