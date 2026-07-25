---
title: 03 · 验证、没做的、小结
---

# 验证、没做的、小结

## 验证

四层。

**第一层:host 单测。** `test/unit/test_fifo.cpp`(FIFO cloning 逻辑、registry)+ `test/unit/test_sys_pipe.cpp` / pipe 单测(nonblock 负测:满 pipe nonblock 写 → PIPE_WOULDBLOCK、空 pipe nonblock 读 → PIPE_WOULDBLOCK、writer 关后 → 0 EOF、ops 映射 WouldBlock)。

**第二层:kernel 测试,端到端。** `kernel/test/test_fifo.cpp` 三例,走真路径(`do_mknod_kernel`/`do_write_kernel`/`do_read_kernel`):mkfifo → FifoRegistry lookup → FifoOps cloning 出写端+读端 → 跨 fd write/read round-trip 内容核对;mknod 非 FIFO → -ENOSYS;重复 mkfifo → -EEXIST。`test_pipe.cpp`/`test_sys_pipe.cpp` 守 SIGPIPE + nonblock。

**第三层:shell 真闭环。** `mkfifo` + `fifotest` 命令在 shell 里跑,用户态端到端验 FIFO。

**第四层:全量。** `run-kernel-test-all` 跑通基线 + SIGPIPE + nonblock + FIFO 各组用例,0 panic / 0 #DF。

> 真阻塞唤醒的正确性靠「对齐 Mutex/console_tty 的 proven 模板 + 代码审查」——harness 单线程没法确定性测两个任务阻塞唤醒,所以负测守住 nonblock 语义 + 旧 pipe 测零回归,真阻塞靠模板复用的信心(同一套 wait queue,Mutex/TTY 已验过跨核 lost-wakeup-safe)。

## 这章没做的

- **open 级两端就绪阻塞**:数据级阻塞(读空/写满)做了;「读写端都 open 了才接通」的 open 级阻塞推迟(本章只做数据级保正确)。
- **close 不销毁 pipe**:close 一个端不拆掉共享 Pipe 开新 epoch——`InodeOps::release` 虚槽已经有了(`inode.hpp:228`),匿名 pipe 两端 ops 的 `release` 也实现了(`pipe_ops.cpp:73/126` 做引用计数,见 `pipe.hpp:124-135`);但 **FIFO cloning 出的 per-open end inode 没人 free**——`FifoOps` 没覆写 `release`,所以 open cloning 时 `new` 出来的 per-open inode + 它的 PipeReadOps/PipeWriteOps 在 close 时仍泄漏(`fifo.cpp:156` 注释仍写「intentional leak on close」)。同匿名 pipe 的 hobby-OS 限制。
- **FIFO flat 命名空间**:都落 `/dev` flat,不在任意路径。任意路径 mkfifo + 用户态 shell 真闭环要 `do_openat_kernel` 也调 open cloning,留后续。
- **ConditionVariable 抽象**:这一章复用现成 wait queue,没抽独立的 ConditionVariable(留到以后讲 sync 那卷)。

## 小结

- 匿名 pipe 两个硬伤:阻塞用 sti/hlt 自旋(syscall 里 sti → 时钟中断抢栈陷阱帧 → #DF,跟 059 sys_ping 同根,harness 假绿盖着);只能亲缘用。
- 阻塞改真调度等待队列(`prepare_to_wait`+`schedule_blocked`+`unblock`,复用 Mutex/console_tty proven 模板,lost-wakeup-safe);Pipe 加 read/write_waiters_。
- BrokenPipe/SIGPIPE(write 已关读端 → -EPIPE + SIGPIPE,靠 reader_alive 区分)+ O_NONBLOCK(满/空返 PIPE_WOULDBLOCK → -EAGAIN,不改 InodeOps 签名,nonblock 收 ops 成员)。
- 命名 FIFO = 给 pipe 起名:FifoRegistry(名字→FIFO)+ cloning open(首读者建共享 Pipe,per-open 端,跟 /dev/ptmx 同套路)+ sys_mknod/mkfifo(S_IFIFO)+ DevFS 节点。shell mkfifo/fifotest 端到端。
- 诚实边界:open 级两端就绪阻塞推迟、close 不销毁 pipe、FIFO flat /dev、ConditionVariable 留到以后讲 sync 那卷。
