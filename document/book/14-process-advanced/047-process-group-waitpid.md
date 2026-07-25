---
title: 047 · 进程组与 waitpid 阻塞
---

# 047 · 给一组进程发信号,让父进程睡等子进程:进程组与 waitpid 阻塞

> 两件事,都是前面留的。第一,`kill` 只能给**单个**进程发信号;可 shell 要做任务控制,得能给**一整组**进程发信号(比如 Ctrl+Z 停掉前台整组)。这需要**进程组**。第二,045 的 `waitpid` 是**轮询**——父进程想知道子进程退没退出,得反复调用、忙等;这既浪费 CPU,又不能及时知道。这一章让父进程能**阻塞睡等**子进程退出。

## 进程组:进程的"编组"

每个进程属于一个**进程组**(pgid),进程组属于一个**会话**(sid)。`Task` 加这几个字段。继承规则:内核 / 引导任务的 pgid 是 0(标记"根"),它 fork 出的第一个用户进程**自成一组**(自身组长);之后用户进程 fork,子继承父的组。

身份操作有四个 syscall:`setpgid`/`setsid`/`getpgid`/`getsid`。有了组,就能**按组发信号**——`killpg(pgid, sig)` 遍历所有任务,给 pgid 匹配的都投递(`kernel/proc/signal.hpp`)。`pgid==0` 是约定"调用者自己的组",方便 shell 给自己所在组发信号。

这一章闭环了 045 留的 `sys_kill(pid<0)`:负 pid 解析成"给进程组 `-pid` 发信号",经 `killpg` 广播。于是 `kill -SIG -pgid` 这类任务控制操作有了内核支撑。

## waitpid 阻塞:睡等,别忙等

这是这一章最绕的部分,因为它牵出一条**依赖链**,不是一个改动能搞定。

原来 `sys_exit` 把任务直接设成"死"(Dead),调度器再也不理它。可父进程要 reap(收尸)的,是"已退出、但还没被收尸的子进程"——如果子进程一退出就消失,父进程就收不到了。所以:

- **exit 改成"僵尸"(Zombie)**:退出的任务不立刻消失,留成 Zombie 状态,等父进程收尸;
- **调度器跳过 Zombie**:`pick_next` 不能选中 Zombie(否则把一个僵尸设成运行态,必崩)。僵尸留在队列里等收尸,但不参与调度;
- **waitpid 默认阻塞**:父进程调 `waitpid`、子进程没退出时,父进程**睡等**(进入等待状态),不再轮询;带 `WNOHANG` 标志才是非阻塞;
- **exit 唤醒父进程**:子进程 exit 时,唤醒正在睡等它的父进程。

这条链远超"给 waitpid 加个阻塞"——所以这一章**拆成两步降风险**:先把"僵尸契约"(exit 留僵尸 + 调度器跳僵尸)做对,再上 waitpid 阻塞 + 唤醒。**改默认阻塞之前,必须 grep 全部 waitpid 调用点,确认它们都能处理"子进程还没退出"的情况**(否则会实打实地挂死在那里)。

> 一个调度器的坑(自 M4-1 起 SMP 化):每个 CPU 各有独立的 `current`,`Scheduler::current()` 和 `Scheduler::set_current()` 读写的是**本 CPU 的同一个 per-CPU 槽** `percpu()->current`——并没有一个全局静态 `current`。所以测试里要设当前任务,用 `set_current` 就行(它直接写 `percpu()->current`);别假设存在一个跨 CPU 共享的静态 `current`,否则经过 `current()` 的路径(waitpid、killpg 都用)在别的 CPU 上会读到另一份旧值。

## 验证

```bash
grep -rn 'killpg\|setpgid\|setsid\|pgid\|session_leader' kernel/proc/signal.hpp kernel/proc/process_group.cpp kernel/proc/process.hpp
grep -rn 'Zombie\|wait_next\|wait_queue_head\|WNOHANG' kernel/proc/ | grep -v test | head
```

构建:

```bash
cmake -B build -S . && cmake --build build -j$(nproc) > /tmp/b.log 2>&1; echo "build=$?"
```

端到端:`killpg` 给一组发信号(组内进程都收到);父进程 `waitpid` 阻塞睡等,子进程 exit 时父进程被唤醒、收尸。真 shell 的任务控制(Ctrl+Z、bg/fg)要等后面的终端(TTY)弧把 SIGSTOP/SIGCONT 接到按键上——但内核侧"按组发信号 + 睡等子进程"的能力,这一章到位了。
