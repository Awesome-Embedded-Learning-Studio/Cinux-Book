---
title: 02 · 代码路线:create_shell_terminal / 私有 pipe / 私有 FDTable / 收尸 / gui_worker / 不要 sti
---

# 代码路线:create_shell_terminal 的全链路

## create_shell_terminal:从 fork 到 jump_to_usermode 的全链路

[gui_init.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/gui/gui_init.cpp) 的 `create_shell_terminal()` 是这一章的重头戏。它先造窗口、造私有管道,然后 fork:

```cpp
int child_pid = cinux::proc::fork(cinux::proc::g_pid_alloc);
if (child_pid > 0) {
    // 父:记下这个终端对应的 shell 进程
    term->set_shell_pid(child_pid);
} else if (child_pid == 0) {
    // ---- 子进程路径 ----
    ...
}
```

子进程这条路径,我们一步步来,有几步缺一不可。**先 `cli`**:接下来要动地址空间、fd 表这些「进程身份」,不能被中断/调度中途插进来。**新建 AddressSpace**:注意父进程(`gui_worker`)是个**没有用户地址空间的内核线程**,所以 034 fork 里那段「CoW 复制父进程页表」在此**根本不执行**(`parent->addr_space == nullptr`)——子进程是从零开始建自己的地址空间:

```cpp
__asm__ volatile("cli");
auto* task = cinux::proc::Scheduler::current();
task->addr_space = new cinux::mm::AddressSpace();
```

这其实点出一件有意思的事:多终端的 fork 是「从内核线程 fork 出用户进程」,不是经典的「从用户进程 fork 出用户进程」。所以 035 费大力气通电的 CoW 页表复制,在这个场景里**用不上**(父没有用户页可复制);CoW 真正派上用场,是以后「用户进程 fork 用户进程」时。但 CoW 的 `handle_cow_fault` 接进 `#PF`、FLAG_USER 过滤这些 035 的通电工作仍然必需——子进程跑起来后,它自己的用户页在被 fork(或被别的机制共享)时就要靠这套。

## 每个终端一对私有 pipe

033 用的是全局管道,所有终端共享一个 shell。035b 我们给每个终端 new 出**自己的一对** `Pipe`:

```cpp
// stdin 管道:Terminal.on_key() 写 → shell 从 fd0 读
auto* stdin_pipe  = new cinux::ipc::Pipe();
auto* stdin_read_ops  = new cinux::ipc::PipeReadOps(stdin_pipe);
auto* stdin_read_inode = new cinux::fs::Inode();
stdin_read_inode->ops = stdin_read_ops;

// stdout 管道:shell 写 fd1 → Terminal.poll_output() 读
auto* stdout_pipe = new cinux::ipc::Pipe();
auto* stdout_write_ops = new cinux::ipc::PipeWriteOps(stdout_pipe);
auto* stdout_write_inode = new cinux::fs::Inode();
stdout_write_inode->ops = stdout_write_ops;

term->set_stdin_pipe(stdin_pipe);
term->set_stdout_pipe(stdout_pipe);
```

每个 `Pipe` 用 `PipeReadOps`/`PipeWriteOps` 包成一个 `Inode`——这是 031b 的套路(把管道伪装成文件,好让 VFS 的 read/write 走它)。但 031b 是全局一对;035b 是**每终端一对**。这就是「独立」的物理基础:A 终端的 stdin_pipe 和 B 终端的 stdin_pipe 是两个不同的 `Pipe` 对象,数据永远串不到一起。

## 私有 FDTable:fd0/fd1 各绑自己的 pipe

光有私有 pipe 还不够,shell 得**通过自己的 fd 表**才能碰到它们。子进程 fork 后建一个全新的 `FDTable`,把 fd0/fd1 绑到**自己这对** pipe 的 inode 上:

```cpp
task->fd_table = new cinux::fs::FDTable();              // 私有 fd 表
task->fd_table->set(0, new cinux::fs::File(stdin_read_inode,  0, cinux::fs::OpenFlags::RDONLY));
task->fd_table->set(1, new cinux::fs::File(stdout_write_inode, 0, cinux::fs::OpenFlags::WRONLY));
```

这里有个和 031b 一脉相承的细节:用 `FDTable::set(0, ...)` / `set(1, ...)` 而不是 `alloc()`——因为 `alloc()` 会跳过 0/1/2(留给标准流),而这里就是要**显式**占据 fd0/fd1。之后这个 shell 的 `sys_read(0)` 走它的 stdin 管道、`sys_write(1)` 走它的 stdout 管道,和别的 shell 的 fd 表毫无关系。fd 表是 per-process 的,这是「每个 shell 独立」的最后一道隔离。

## Terminal 持 shell_pid,析构 waitpid 收尸

[terminal.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/gui/terminal.hpp) 给 `Terminal` 加了 `shell_pid_`(初值 0,表示没绑 shell)。父进程 fork 后立刻 `set_shell_pid(child_pid)` 把它记下。这一笔看似不起眼,却是生命周期闭合的关键:关掉终端窗口时,`Terminal` 析构用它来收尸:

```cpp
if (shell_pid_ > 0) {
    for (uint32_t attempt = 0; attempt < 1000; attempt++) {
        int status = 0;
        auto result = cinux::proc::waitpid(shell_pid_, &status, cinux::proc::g_pid_alloc);
        if (result == WaitpidResult::Ok) { kprintf("[TERM] Reaped shell pid=%d status=%d\n", ...); break; }
        if (result == NoChildren || result == NotFound) break;
        // NotExited:自旋重试(034 的 waitpid 是非阻塞的)
    }
    shell_pid_ = 0;
}
```

为什么是「有界循环重试」而不是一次阻塞 `waitpid`?因为 034 的 `waitpid` 是**非阻塞**的(孩子没退就返回 `NotExited`)。这里用最多 1000 次的自旋把它凑成一个「近似阻塞」的收尸:shell 多半很快退(用户关窗口时它往往已经没事干),几轮就能 Ok;万一卡住也有上限,不会把析构拖死。这一段把 034 的 `waitpid`、`set_shell_pid`、防 zombie 全串了起来——是整条进程弧的收口。

## 为什么 fork/execve 要延迟到 gui_worker

`create_shell_terminal` 不是在 PIT 滴答回调里直接调的。因为滴答回调跑在**中断上下文**,而 fork/execve 是一串带内存分配、地址空间切换、调度器改动的重活——在中断里干这些既不安全、也容易和调度器状态打架。

所以 035 把它挪到一个专门的 `gui_worker` **内核线程**:点图标时(中断上下文)只往一个 `std::atomic<IconAction> g_pending_action` 里投递动作,`gui_worker` 线程排空这个标志、在**线程上下文**里调 `create_shell_terminal`。这又是 Cinux GUI 一贯的「中断端只记录、主循环/工作线程消费」——只不过这次的「消费端」从滴答回调升级成了一个专职内核线程,因为活儿重到滴答里干不动了。

## 那个「Do NOT sti」

子进程进用户态前有一段注释值得记住:`jump_to_usermode` 之前**不要** `sti`。直觉上「要开中断了,sti 一下」很自然,但这里不行:

```text
// Do NOT sti here. SYSRETQ restores R11 into RFLAGS, and R11
// already has IF=1 (set in jump_to_usermode), so interrupts
// are enabled atomically upon entering Ring 3. An explicit
// sti before SYSRETQ opens a window where the PIT fires on
// the child's CR3, causing gui_tick_callback to composite
// with incomplete identity mappings -- demand-paging zero
// pages over the framebuffer MMIO region.
```

`SYSRETQ` 会用 R11 恢复 RFLAGS,而 R11 此时已带 `IF=1`,所以进 Ring 3 的那一刻中断是**原子地**打开的。若在 `SYSRETQ` 前手贱 `sti`,就开了一扇窗:PIT 在子进程的 CR3(地址空间还没完全建好)上触发,`gui_tick_callback` 去合成画面,撞上不完整的映射,把零页 demand-page 到 framebuffer 的 MMIO 区——画面炸。这种「多一步 sti 反而坏事」的细节,是真实踩出来的。
