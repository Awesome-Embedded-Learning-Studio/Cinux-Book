---
title: 046 · clone / futex / TLS
---

# 046 · clone、futex、TLS:真正的线程

> 045 给了信号,046 给**线程原语**。`fork` 是"整个进程复制一份",而 Linux 风格的 `clone` 是"按 flag 决定哪些共享、哪些复制"——共享地址空间(`CLONE_VM`)时,生出来的就是**线程**。配上 `futex`(线程同步)、TLS(每线程局部存储)、cleartid(`pthread_join` 协议),POSIX 线程的底子就齐了。A 档:`clone` 起两个线程交错跑,是真做得到的。

## 这章咱们要点亮什么

四件事:

1. **`sys_clone`(Linux 56)**:按 `CLONE_*` flag 决定共享/复制;`CLONE_THREAD` 的兄弟同 `tgid`(`getpid` 返组长 pid)。
2. **TLS**:`CLONE_SETTLS` 设子线程的 `fs_base`,context_switch 切入时恢复(MSR_FS_BASE)。
3. **`futex`**:wait/wake,userspace fast-path 同步原语。
4. **cleartid**:`CLONE_CHILD_CLEARTID` —— 线程 exit 时清 `child_tid` + `futex_wake`,这就是 `pthread_join` 的内核侧。

## fork vs clone

`fork` 把整个进程复制一份(地址空间 CoW、fd_table/sig_actions/cwd 各拷一份)。`clone` 细粒度——按 flag 决定:

- `CLONE_VM` → 共享地址空间指针(这就是**线程**——两个执行流跑在同一套页表上);
- `CLONE_FILES` → 共享 fd_table;`CLONE_SIGHAND` → 共享 sig_actions;`CLONE_FS` → 共享 cwd;
- 不带这些 flag → 复制(fork 语义);
- `CLONE_THREAD` → 兄弟关系(同 tgid,ppid 继承自父的 ppid,不入调用者 children)。

`Task` 加了线程组字段:`tgid`(= 组长 pid,`getpid` 返它)、`group_leader`、`clear_child_tid`/`set_child_tid`。核线程 tgid=0;fork 给子新 tgid(自身组长);`clone(CLONE_THREAD)` 给子父的 tgid(兄弟)。

## clone 实现:镜像 fork,按 flag 共享

`clone()`(`kernel/proc/fork.cpp`)镜像 fork 的"new Task + memcpy + 拷核栈 + ctx + `fork_child_trampoline`(rax=0)",然后按 flag 决定 share-or-copy。共享资源用 refcount 指针(acquire);复制用 create_copy。

> 这一弧之前(F3-M2 批 3)先把 `sig_actions`/`cwd`/`fd_table` 从"深拷贝"改成 **refcount 共享对象**,clone 才能按 flag 真共享——否则共享指针指向的会被一边析构。这是 clone 能落地的前置。

### GOTCHA #18:子进程的用户栈返回

clone 最硬的一处。fork 子进程返回**父栈**(CoW 共享);但 clone 子进程要返回**调用者给的 `stack`**(线程有自己栈)。怎么办?

syscall 入口建的 pt_regs 帧固定在 `[kernel_stack_top-96, kernel_stack_top)`(96B、12 槽),`user_rsp` 在帧的 offset 0 = `kernel_stack_top-96`。clone 复用 fork 的"拷父核栈"机制(子核栈是父栈副本,含 syscall 帧),然后**直接 patch 子帧的 user_rsp 槽**:

```cpp
if (stack != 0)
    *(uint64_t*)(child->kernel_stack_top - 96) = stack;
```

子进程经 `fork_child_trampoline(rax=0)` 解卷回 syscall 入口岭 → SYSRET 时 user_rsp=stack、user_rip=父的岭(`CLONE_VM` 共享代码)、rax=0。**帧在栈顶固定位置,直接按 `kernel_stack_top` 定位,不用从当前 rsp 算偏移。**

## TLS:fs_base

`CLONE_SETTLS` → `child->ctx.fs_base = tls`(`kernel/proc/process.hpp:92`)。`fs_base` 是每线程的 TLS 基址(MSR_FS_BASE 0xC0000100),`context_switch` 切入时恢复——所以每线程的 `%fs` 段指向各自的 TLS 区。task_builder 默认 `fs_base=0`(没 TLS 直到 clone 带 SETTLS)。

## futex:wait/wake

`sys_futex(uaddr, op, val, ...)`(`kernel/syscall/sys_futex.hpp:39`):`FUTEX_WAIT` 在 `*uaddr==val` 时阻塞、`FUTEX_WAKE` 唤醒等者。内核侧维护 per-uaddr 的等待队列。它是 pthread mutex/condvar 的底——userspace 先原子检查(fast-path,不进内核),冲突才 futex(慢路径)。

## cleartid:pthread_join 的内核侧

`CLONE_CHILD_CLEARTID` 记一个 `clear_child_tid`(用户地址)。线程 exit 时 `task_exit_cleartid`(`process.hpp:381`)把这个地址**清零 + `futex_wake` 一个等者**——这正是 `pthread_join` 的协议:join 的线程在 cleartid 地址上 futex_wait,被 join 的线程 exit 时清零 + wake。零额外机制,复用 futex。

## 一个值得记的调试 saga

这一弧踩到一个**极具迷惑性**的坑,记一下。批 4 改 `sys_getpid` 让它返 `tgid` 后,整套测试挂死,崩点显示在 `FDTable::alloc` 的 Spinlock,诊断打印 `current_fd_table: fd_table=0x0AFFFFFF81072946`(垃圾),像是内存踩踏。

真因**不是踩踏,是悬垂指针**:getpid 返 tgid 使一条既有断言失败(`tmp.pid=42` 但 `tmp.tgid=0` → getpid 返 0 ≠ 42);`TEST_ASSERT` 失败时 `return`(早返回)→ **跳过函数末尾的 `set_current(prev)`** → `Scheduler::current_` 悬垂指向已销毁的栈 `Task tmp` → 后续测试读悬垂 current → fd_table 垃圾 → 崩。修:测试设 `tmp.tgid = tmp.pid`。

> 教训:**测试 helper 改了 `current` 必须配对恢复**;`TEST_ASSERT` 的早返回会跳过清理。诊断这种"看似踩踏实为悬垂"的 bug,关键是在崩点打印 `current` 指向的 task 地址——垃圾地址 = current 悬垂。

## 验证

```bash
grep -rn 'sys_clone\|sys_futex' kernel/syscall/
grep -rn 'fs_base\|task_exit_cleartid\|CLONE_SETTLS\|CLONE_CHILD_CLEARTID' kernel/proc/process.hpp
```

构建 + 内核测试(这一弧 run-kernel-test 从 045 的 783 涨到 809,clone/futex/TLS/cleartid 一批):

```bash
cmake --build build -j$(nproc) > /tmp/b.log 2>&1; echo "build=$?"
cmake --build build --target run-kernel-test
```

A 档端到端:测试里 `clone(CLONE_VM|CLONE_THREAD|CLONE_SETTLS, ...)` 起兄弟线程,两线程同 tgid、各跑各的 TLS,futex 协调。真用户态 pthread 程序要等 F10 musl 的 libpthread。

## 小结与下一站

线程原语齐了:clone(按 flag 共享/复制 + 线程组)、TLS(fs_base)、futex(同步)、cleartid(join)。POSIX 线程的内核底子铺好。

下一站 **047** 继续进程弧:进程组 / 会话 / `waitpid` 阻塞——`kill(pid<0)` 给进程组发信号、父进程阻塞等子进程退出。
