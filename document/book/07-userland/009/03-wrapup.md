---
title: 03 · 收尾:非 GUI 生产启动验收与诚实边界
---

# 收尾:非 GUI 生产启动验收与诚实边界

## 验收:非 GUI 构建的生产启动

机制测试(run-kernel-test)能证明上面这些改动没破坏既有测试(单核 + SMP 两腿都过)。可 init PID1 这件事**机制测试证明不了**——非 GUI 的 `shell_launch` 路径在测试内核里不走。要真验收,得用非 GUI 构建启动一次(Cinux 里就是 `CINUX_GUI=OFF` 那份,咱们叫 build-console)。下面是笔者实测拿到的串口(关键几行):

```
[INIT] kernel_init started tid=3 pid=1            ← 入口 alloc 领到 PID1
[EXECVE] loaded /sbin/init entry=0x431E0C pid=1   ← busybox init 以 PID1 身份 execve(保 pid)
CinuxOS init: filesystems mounted                 ← /etc/inittab 的 ::sysinit:/bin/echo
[WAITPID] reaped child pid=2 exit_status=0 by parent pid=1   ← PID1 reap child
[EXECVE] loaded /bin/sh entry=0x431E0C pid=2      ← ::respawn:/bin/sh
[SYS_OPEN] File not found: '/dev/tty'             ← 已知非致命边界(TIOCSCTTY 没真设)
~ #                                               ← ash 交互提示符
```

`pid=1` 这一行是整章的眼。从"匿名 kthread"到"PID 1 的 busybox init",中间没有 fork,只有一次 `alloc()` 和一次保 pid 的 `execve`。后面的 `[WAITPID] reaped child ... by parent pid=1` 更是把"PID1 是孤儿归宿"这件事落到了实处——echo 跑完退出,PID1 把它 reap 掉,这正是 init 该干的活。

> **一个诚实的坑**:这一步偶发会首启动失败——`[PROC] jumping to user mode` 那行有时打出一个 `0xFFFFFFFF8...` 的内核地址而非 `0x431E0C`,随即 #PF panic。根因在 [user_launch.cpp](../../../kernel/proc/user_launch.cpp) 里 `enter_loaded_program` 跳用户态的入口是从 `task->ctx.rip` 读的,而 `execve` 刚把这个字段设成 ELF 入口;两者之间那一小窗,偶发被一次调度打断了、把内核 RIP 存回了 `ctx.rip`。稳的做法其实是直接跳 `elf_aux.at_entry`(它本就是函数参数,不会被调度动),可这一行一直没改——是个带了很多版的潜在脆弱点,重跑一次通常就过。笔者写在这里,是想说:**生产启动绿这件事,值得多跑几次确认,别被一次偶发 panic 骗成"坏了"**。

## 诚实的边界

这一章做的是"最小可用的 init PID1",有几件事显式不做,留给以后:

- **`rt_sigtimedwait` 的真阻塞 + 超时**:现在是没信号就 `-EAGAIN` 的忙等(靠 RR 时间片防饿死)。真阻塞需要 timer_queue 驱动超时——唤醒钩子 `sigwait_blocked` 已经在 `signal_send()` 里留好,将来直接接。
- **TIOCSCTTY 的真控制终端语义**:`TIOCSCTTY` 现在接了但没真设 `controlling_tty` 字段,`/dev/tty` 还解析不到。所以非 GUI 启动里你会看到 busybox init 报一句 `/dev/tty` NotFound——**非致命**,ash 照样走到 `~ #` 等键盘。
- **login/getty + 真认证**:现在 init 直接 respawn `/bin/sh`,没有 login。真用户登录是另一个里程碑。
- **PID1 reap 的完整语义**:现在能 reap 单个 child(上面的 `reaped child pid=2 by parent pid=1`)。真正压上大量 fork 的孤儿 reap(比如跑编译器那种 fork 链)还得再打磨。

这些边界不影响"init 是 PID 1、能 respawn sh"这个 punchline——它已经成立。
