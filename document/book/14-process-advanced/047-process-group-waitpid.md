---
title: 047 · 进程组与 waitpid 阻塞
---

# 047 · 进程组、会话、waitpid 阻塞:给 job control 打地基

> 047 收 F3 的两块:**进程组/会话身份** + **waitpid 阻塞**。前者让 `kill` 能给一组进程发信号(`kill(pid<0, sig)`)、为 job control / TTY 控制终端打地基;后者让父进程能**阻塞等子进程退出**(045/046 的 waitpid 还是轮询)。A 档:`killpg` 广播、`waitpid` 阻塞,都是用户可见的行为。

## 这章咱们要点亮什么

1. **进程组/会话身份**:`Task` 加 `pgid`/`sid`/`session_leader`/`controlling_tty`;fork/clone 的继承规则集中到 `inherit_process_identity`。
2. **身份操作 + 信号广播**:`setpgid`/`setsid`/`getpgid`/`getsid` 四个 syscall + `killpg`(按 pgid 广播信号),闭环 045 留的 `sys_kill(pid<0)`。
3. **waitpid 阻塞 + Zombie reap**:exit 从"直接 Dead"改成"Zombie + 留给父 reap";waitpid 默认阻塞、`WNOHANG` 非阻塞;exit 唤醒等子进程的父。

## 进程组/会话身份

`Task` 加一组字段:`pgid`(进程组 id)、`sid`(会话 id)、`session_leader`、`controlling_tty`。继承规则集中到 `inherit_process_identity`(`kernel/proc/process_new.cpp`):

```cpp
// process_new.cpp:213 —— root fork:父 pgid==0(内核/bootstrap task),子自成组长
if (parent->pgid == 0) {
    child->pgid = child_pid;
    child->sid  = child_pid;
    // session_leader = true
} else {
    // 否则继承父的 pgid/sid
}
```

"root fork"是关键概念:内核/bootstrap task 的 `pgid==0`,它 fork 出的第一个用户进程自成一组(自身组长);之后用户进程 fork,子继承父的组。这和 Linux 一致。

## 身份操作 + killpg

`kernel/proc/process_group.{hpp,cpp}` 提供 `setpgid`/`getpgid`/`getsid`/`setsid`(纯字段操作,`setsid` 在调用者已是 leader 时 EPERM),注册成四个 syscall(109/112/121/124)。

信号广播 `killpg`(`kernel/proc/signal.hpp:209`):

```cpp
// 遍历 pid registry,给所有 pgid 匹配的 task 发信号;pgid==0 解析为调用者自己的组
int killpg(int pgid, Signal sig);
```

这一弧闭环了 045 留的 `sys_kill(pid<0)`:`pid<0` 解析成"给进程组 `-pid` 发信号",经 `killpg` 广播。于是 shell 的 `kill -SIG -pgid` 这类 job control 操作有了内核支撑。

## waitpid 阻塞 + Zombie reap

这是这一弧最绕的一块,藏着依赖链。045/046 的 `waitpid` 是**轮询**(子没退出就返"没有"),父要自己忙等。047 改成默认**阻塞**。

但"阻塞"不是只改 waitpid——它牵出一串前置:

- **exit 改 Zombie 契约**:原来 `sys_exit` 直接把 task 设 Dead;改成 **Dead→Zombie**(留 task 结构,等父 reap)。因为父 reap 的是"已退出但还没被收尸的子",直接消失就 reap 不到真 child。
- **scheduler 跳过 Zombie**:`pick_next` 不能选 Zombie 状态的 task(否则把 Zombie 设成 Running → 崩)。Zombie 留在队列里等 reap,但不参与调度。
- **waitpid 阻塞**:默认 block(`waiting_for_child`),`WNOHANG` 才非阻塞;阻塞后重扫 loop。
- **exit 唤醒父**:子 exit 时唤醒 `waiting_for_child` 的父。

这条链远超"加个 block"——所以这一弧**拆成两批降风险**:4a(契约:exit Zombie + scheduler 跳 Zombie)先行,4b(waitpid 阻塞 + 唤醒)在后。

> 教训:**propose 时以为是一个改动,执行中发现是依赖链**——waitpid 阻塞的正确性依赖 exit Zombie + scheduler skip + 所有调用点改 WNOHANG。这种时候拆批(契约先行、行为在后)比一把梭稳。改默认阻塞前,务必 `grep` 全 waitpid 调用点确认 WNOHANG/zombie 就绪,否则 `test_waitpid_not_exited`(child 还 Running)会实打实挂死。

## GOTCHA #21:Scheduler::current 读的是 static

`Scheduler::current()` 读的是 static `current_`,**不是 `g_per_cpu.current``(单核期这俩不同步)**。单测里设 current 必须用 `Scheduler::set_current(&t)`(两个都设),不能直接 `g_per_cpu.current=&t`。test_clone 的 futex 侥幸过了(futex 路径不经 `current()`),掩盖了这层——到 waitpid 才暴露。

## 验证

```bash
grep -rn 'killpg\|setpgid\|setsid\|pgid\|session_leader' kernel/proc/signal.hpp kernel/proc/process_group.cpp kernel/proc/process.hpp
grep -rn 'Zombie\|waiting_for_child\|WNOHANG\|kWaitNoHang' kernel/proc/ | grep -v test | head
```

构建 + 内核测试(这一弧 run-kernel-test 从 046 的 809 涨到 827):

```bash
cmake --build build -j$(nproc) > /tmp/b.log 2>&1; echo "build=$?"
cmake --build build --target run-kernel-test
```

A 档端到端:`killpg` 给一组发信号(组内 task 都收到)、`waitpid` 阻塞等子退出(子 exit 时父被唤醒)。真 shell 的 job control 要等 F10 + TTY(后面)。

## 小结与下一站

F3 进程弧近收口:进程组/会话身份有了,waitpid 能阻塞了。还差 F3-M4(调度类:SIGSTOP/CONT 真调度效果、优先级、task 状态机)。

下一站 **048** 就是 F3-M4 调度器——`SchedulingClass` 策略钩子 + 优先级轮询 + SIGSTOP/CONT 真把任务停/续。
