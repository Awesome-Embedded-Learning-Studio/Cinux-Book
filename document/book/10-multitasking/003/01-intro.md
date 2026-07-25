---
title: 01 · 导引:点亮什么、为什么、设计图
---

# 导引:点亮什么、为什么、设计图

> 035 那一章我们把 fork/exec 彻底通了电——子进程会返回 0、CoW 真的写时复制、syscall 跨切换正常。但那套能力还晾在那儿,没人用。这一章,我们把它接进 GUI:点桌面上的终端图标,内核就 fork 出一个子进程、让它 execve 成 `/bin/sh`、跳进用户态跑起来。关键在于——**每个终端窗口背后是一个各自独立的 shell 进程**,你在 A 终端敲命令,B 终端纹丝不动。这是整条 GUI/多任务弧的高潮,也是 Cinux 从「一个进程跑到底」走到「真正的多进程桌面」的终点。

## 这一章我们要点亮什么

一件最能收尾的事:开机进桌面,点 shell 图标,弹出一个终端、里面是一个全新的 shell(`cinux> ` 提示符、能执行命令);再点一次,弹出**第二个**终端、里面是**另一个独立的** shell。你在两个终端里各敲各的,它们互不串扰——A 里 `pwd` 不会在 B 里冒出来。

这一幕背后,是 035 通电的 fork/exec 第一次被**真正用起来**:

- **点图标 → 生一个 shell 进程**。`create_shell_terminal()` 调 `fork()`,父进程(GUI 工作线程)继续、子进程去 `execve("/bin/sh")`。
- **每个终端一对私有管道**。不是 033 那种「所有终端共享一对全局管道」,而是每个终端 new 出自己的 stdin/stdout `Pipe`,这个 shell 的输入输出只流进**它自己的**终端。
- **每个 shell 一张私有 fd 表**。子进程 fork 后建一个全新的 `FDTable`,把 fd0 绑自己的 stdin 管道、fd1 绑自己的 stdout 管道——于是 `read(0)` / `write(1)` 只碰自己的终端。
- **终端关掉时收尸**。关一个终端窗口,它的 `Terminal` 析构会 `waitpid` 自己的 shell,把僵尸收掉,不留泄漏。

> 一句话:033 让「点图标开终端」成立,但所有终端共用一个 shell;035b 用 fork/execve 让「每个终端一个独立 shell」成立。中间隔的就是 034 的进程原语和 035 的通电。

## 为什么现在需要它

回看 033 留下的遗憾:点终端图标能开窗口,但 `create_shell_terminal` 接的是**全局** `g_stdin_pipe`/`g_stdout_pipe`——也就是 031b 给那一个 shell 建的管道。于是你开两个终端窗口,它们 `set_stdin_pipe`/`set_stdout_pipe` 接的是**同一对管道**,背后是**同一个 shell**。在 A 窗口敲命令,B 窗口的输出跟着动;两个窗口都不是独立终端,只是同一个 shell 的两个取景器。

要做成真正的多终端,得让「开一个终端」=「生一个新的 shell 进程出来」。而「生进程」正是 034 的 fork、034+035 把它通上了电。所以 035b 我们要做的事很明确:把 `create_shell_terminal` 从「new 一个 Terminal 接全局管道」改造成「fork 一个子进程、给它一对私有管道、让它 execve 成 shell」。

## 设计图

`create_shell_terminal` 的全链路,是这一章的核心:

```text
  create_shell_terminal()(在 gui_worker 内核线程里跑,不在中断上下文)
        │
        ├─ new Terminal("Shell #N")、set_font、算尺寸/居中
        │
        ├─ 建这一对私有管道:
        │     stdin_pipe  + PipeReadOps  → Inode(stdin_read_inode)
        │     stdout_pipe + PipeWriteOps → Inode(stdout_write_inode)
        │   term->set_stdin_pipe/stdout_pipe(内核侧直接访问)
        │
        ├─ fork()
        │     │
        │     ├─ 父(child_pid > 0):term->set_shell_pid(child_pid)  ← 终端记住自己的 shell
        │     │
        │     └─ 子(child_pid == 0):
        │           cli
        │           task->addr_space = new AddressSpace()        ← gui_worker 父是无地址空间内核线程
        │           task->fd_table = new FDTable()                ← 私有 fd 表
        │              set(0, File(stdin_read_inode,  0, RDONLY))    ← fd0 = 自己的 stdin 管道
        │              set(1, File(stdout_write_inode, 0, WRONLY))   ← fd1 = 自己的 stdout 管道
        │           execve("/bin/sh", argv, envp)                 ← 换成 shell 映像
        │           建用户栈(USER_STACK_PAGES 页)
        │           task->addr_space->activate()                  ← 切到子进程页表
        │           g_per_cpu.update_syscall_stack(kernel_stack_top)  ← syscall 用子进程内核栈
        │           jump_to_usermode(entry, USER_STACK_TOP - USER_ABI_RSP_OFFSET, 0)  ← 进 Ring 3,不返回
        │
        └─ wm.add_window(term)   ← 把这个终端窗口摆上桌面
```

「为什么每个终端独立」全在这张图里:每个终端各自 fork、各自的子进程各自建私有 `AddressSpace` + 私有 `FDTable` + 各自的 pipe 对。两个终端 = 两个 shell 进程 = 两套 fd = 两对管道,物理上隔离。

终端的生命周期闭合靠析构里的收尸:

```text
  关闭终端窗口 → Terminal 析构:
     if (shell_pid_ > 0)
         有界循环(最多 1000 次): waitpid(shell_pid_, &status, g_pid_alloc)
             Ok        → 打 [TERM] Reaped shell pid=... status=... ; 跳出
             NoChildren/NotFound → 跳出
             NotExited → 自旋重试(非阻塞 waitpid)
         shell_pid_ = 0
```
