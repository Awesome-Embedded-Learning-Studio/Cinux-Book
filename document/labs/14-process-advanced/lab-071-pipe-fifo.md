---
title: Lab 071 · Pipe 增强与命名 FIFO 验证
---

# Lab 071 · Pipe 增强与命名 FIFO 验证

> 对应 `document/book/14-process-advanced/071-pipe-fifo.md`。验证档 **A 档**:这一章交付的是 pipe 增强(真阻塞/SIGPIPE/O_NONBLOCK)+ 命名 FIFO。punchline 是 FIFO 端到端走通(mkfifo → open 两端 → 跨 fd 往返)+ pipe 写已关读端触发 SIGPIPE。验证靠 host 单测(nonblock 负测 + FIFO cloning)+ kernel 端到端测 + shell mkfifo/fifotest。

## 目标

确认七件事:

1. **sti/hlt 自旋阻塞已除**:改成真调度 wait queue(跟 059 sys_ping #DF 同根的隐患);
2. **wait queue 复用 Mutex/console_tty 模板**(lost-wakeup-safe);
3. **O_NONBLOCK → EAGAIN**(满/空返 PIPE_WOULDBLOCK,不改 InodeOps 签名);
4. **BrokenPipe/SIGPIPE**(写已关读端 → -EPIPE + SIGPIPE,reader_alive 区分);
5. **FIFO = 给 pipe 起名**:FifoRegistry + cloning open(首读者建 Pipe,同 /dev/ptmx);
6. **sys_mknod/mkfifo**(只 S_IFIFO,别的 -ENOSYS);
7. **shell mkfifo/fifotest 端到端** + 两腿 1019/0。

## 步骤

### 1. host 单测

```bash
./build/test/test_fifo && ./build/test/test_sys_pipe
```

应看到 test_fifo 4 passed(FIFO cloning 逻辑、registry)+ test_sys_pipe 全绿(含 nonblock 负测:满 pipe nonblock 写 → PIPE_WOULDBLOCK、空 pipe nonblock 读 → PIPE_WOULDBLOCK、writer 关后 → 0 EOF、ops 映射 WouldBlock)。

### 2. sti/hlt #DF 隐患 → 真 wait queue

```bash
sed -n '12,20p' kernel/ipc/pipe.hpp
```

应看到文件头注释:`prepare_to_wait()/schedule_blocked()/unblock()` pattern(跟 Mutex、console TTY 阻塞读同一个 proven 模板)。这就是替代 sti/hlt 自旋的真调度等待——旧实现 `irq_enable(); for{hlt;...}` 在 syscall 上下文 sti,时钟中断抢 `%gs:0` 栈陷阱帧 → sysretq 弹花 → #DF(跟 059 sys_ping 同根,harness 假绿盖着)。

### 3. O_NONBLOCK

```bash
sed -n '52,52p' kernel/ipc/pipe.hpp
sed -n '84,106p' kernel/ipc/pipe.hpp
```

应看到 `PIPE_WOULDBLOCK = -2` + `read`/`write` 加了 `bool nonblock = false` 参数(默认 false,旧 2 参调用不破;满/空且 nonblock 返 PIPE_WOULDBLOCK)。再看 ops 层映射:

```bash
grep -nE "WouldBlock|nonblock_|PIPE_WOULDBLOCK" kernel/ipc/pipe_ops.cpp | head
```

应看到 ops 持 `nonblock_` 成员,PIPE_WOULDBLOCK 映射 `Error::WouldBlock`(→ -EAGAIN)。**不改 InodeOps::read/write 签名**(blast radius 大,PTY/ext2/DevFS 都实现它)。

### 4. BrokenPipe / SIGPIPE

```bash
grep -nE "reader_alive|BrokenPipe|InvalidArgument" kernel/ipc/pipe_ops.cpp | head
```

应看到 write 返负数时 `reader_alive() ? InvalidArgument : BrokenPipe`——读端没了→BrokenPipe(-EPIPE,sys_write raise SIGPIPE),否则 InvalidArgument。区分这两者是 SIGPIPE 的关键(否则一律 IOError → -EIO,永远不触发 SIGPIPE)。

### 5. FIFO:cloning open(给 pipe 起名)

```bash
sed -n '87,108p' kernel/ipc/fifo.hpp
```

应看到 `FifoRegistry`(in-memory name → FIFO map,单例)+ 注释说「opening that name clones a per-open end」(同 `/dev/ptmx` cloning 套路)。首读者 open 时建共享 Pipe,之后 open clone 出接同一根 Pipe 的 per-open 端——「同名」接「同 Pipe」,每个 open 拿独立端。

### 6. sys_mknod / mkfifo

```bash
sed -n '22,56p' kernel/syscall/sys_mknod.cpp
```

应看到 `do_mknod_kernel`(只接 `S_IFIFO`,char/block → -ENOSYS,本里程碑只做 FIFO)+ `sys_mknod`(走 061 那套 `resolve_user_path` + `do_mknod_kernel` 分层)。`mkfifo(path,mode)` 是 libc 拼写 = `mknod(path, S_IFIFO|mode, 0)`。

### 7. kernel 端到端 + shell + 两腿

```bash
cmake --build build --target run-kernel-test 2>&1 | grep -iE "fifo|pipe" | head
```

应看到 `test_fifo_mkfifo_open_roundtrip`(mkfifo → cloning 出写端+读端 → 跨 fd write/read 核对)等 PASS,加 SIGPIPE/nonblock 测。

shell 真闭环(test kernel 不跑 shell,要起真内核):

```bash
cmake --build build --target run
```

进 shell 试 `mkfifo` 建 FIFO + `fifotest` 端到端验(写端写、读端读、核对)。

两腿汇总:

```bash
cmake --build build --target run-kernel-test-all 2>&1 | grep -E "Tests: [0-9]{4} passed" | tail -1
```

应看到单核 + `-smp 2` 两腿各 `1019 passed, 0 failed`(基线 + SIGPIPE 1 + nonblock 3 + FIFO 3),0 panic / 0 #DF。

## 验收清单

- [ ] `test_fifo` 4 + `test_sys_pipe` 全绿(nonblock 负测 + FIFO cloning host 单测)。
- [ ] `pipe.hpp:15` wait queue(prepare_to_wait/schedule_blocked/unblock,替 sti/hlt 自旋,#DF 隐患消除)。
- [ ] `pipe.hpp:52` PIPE_WOULDBLOCK + `:89`/`:105` read/write `bool nonblock`;ops 映射 WouldBlock,-EAGAIN;不改 InodeOps 签名。
- [ ] `pipe_ops.cpp` write 返负时 `reader_alive?InvalidArgument:BrokenPipe`(-EPIPE + SIGPIPE)。
- [ ] `fifo.hpp:96` FifoRegistry + cloning open(首读者建 Pipe,per-open 端,同 /dev/ptmx)。
- [ ] `sys_mknod.cpp:22` do_mknod_kernel(只 S_IFIFO);shell mkfifo/fifotest;两腿 1019/0。

## 别做这些

- **别**用 sti/hlt 自旋做 pipe 阻塞——在 syscall 上下文 sti,时钟中断抢栈陷阱帧 → sysretq 弹花 → #DF(跟 059 sys_ping 同根,真硬件必炸,harness 假绿盖着)。用真调度 wait queue。
- **别**改 InodeOps::read/write 签名加 nonblock——blast radius 大(PTY/ext2/DevFS 都实现它)。nonblock 收 ops 成员,InodeOps 签名不动。
- **别**把「写已关读端」和「参数错」混成同一个 IOError——前者该 BrokenPipe(-EPIPE + SIGPIPE),后者 InvalidArgument。靠 `reader_alive()` 区分,否则 SIGPIPE 永远不触发。
- **别**以为 close 一个 FIFO 端会拆掉共享 Pipe——没有 InodeOps::release 钩子,close 不销毁 pipe(per-open ops/inode 还会泄漏,hobby-OS 限制)。
- **别**以为 FIFO 能在任意路径——这一章 flat 落 `/dev`(DevFS dynamic_lookup)。任意路径 mkfifo 要 do_openat 也调 cloning open,留后续。
- **别**指望 harness 测真阻塞唤醒——单线程没法确定性测两任务阻塞唤醒。真阻塞靠对齐 Mutex/console_tty proven 模板 + 代码审查;负测守 nonblock 语义。
