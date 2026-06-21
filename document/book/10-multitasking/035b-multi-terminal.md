---
title: 035b · 多终端:每个终端一个独立的 shell
---

# 035b · 多终端:每个终端一个独立的 shell

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

## 代码路线

### create_shell_terminal:从 fork 到 jump_to_usermode 的全链路

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

### 每个终端一对私有 pipe

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

### 私有 FDTable:fd0/fd1 各绑自己的 pipe

光有私有 pipe 还不够,shell 得**通过自己的 fd 表**才能碰到它们。子进程 fork 后建一个全新的 `FDTable`,把 fd0/fd1 绑到**自己这对** pipe 的 inode 上:

```cpp
task->fd_table = new cinux::fs::FDTable();              // 私有 fd 表
task->fd_table->set(0, new cinux::fs::File(stdin_read_inode,  0, cinux::fs::OpenFlags::RDONLY));
task->fd_table->set(1, new cinux::fs::File(stdout_write_inode, 0, cinux::fs::OpenFlags::WRONLY));
```

这里有个和 031b 一脉相承的细节:用 `FDTable::set(0, ...)` / `set(1, ...)` 而不是 `alloc()`——因为 `alloc()` 会跳过 0/1/2(留给标准流),而这里就是要**显式**占据 fd0/fd1。之后这个 shell 的 `sys_read(0)` 走它的 stdin 管道、`sys_write(1)` 走它的 stdout 管道,和别的 shell 的 fd 表毫无关系。fd 表是 per-process 的,这是「每个 shell 独立」的最后一道隔离。

### Terminal 持 shell_pid,析构 waitpid 收尸

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

### 为什么 fork/execve 要延迟到 gui_worker

`create_shell_terminal` 不是在 PIT 滴答回调里直接调的。因为滴答回调跑在**中断上下文**,而 fork/execve 是一串带内存分配、地址空间切换、调度器改动的重活——在中断里干这些既不安全、也容易和调度器状态打架。

所以 035 把它挪到一个专门的 `gui_worker` **内核线程**:点图标时(中断上下文)只往一个 `std::atomic<IconAction> g_pending_action` 里投递动作,`gui_worker` 线程排空这个标志、在**线程上下文**里调 `create_shell_terminal`。这又是 Cinux GUI 一贯的「中断端只记录、主循环/工作线程消费」——只不过这次的「消费端」从滴答回调升级成了一个专职内核线程,因为活儿重到滴答里干不动了。

### 那个「Do NOT sti」

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

## 验证

**第一层:host 单元测试。** 多终端的纯逻辑(host 镜像):

```bash
ctest --test-dir build -R multi_terminal --output-on-failure
```

重点看 `test_multi_term_two_terminals_independent_pipes` 这类用例——它验的就是「两个终端的管道互不串扰」。

**第二层:QEMU kernel 测试。** 真 fork/execve/pipe 的机内测:

```bash
cmake --build build --target run-big-kernel-test
```

`run_multi_terminal_tests()` 把多终端逻辑放到真内核 + QEMU 里验,最终 `ALL TESTS PASSED`。

**第三层:端到端(整条弧的验收)。** 这是 Cinux GUI/多任务之旅的终点,值得亲眼看:

```bash
cmake --build build --target run
```

预期:进桌面,点 shell 图标,弹出一个终端、里面是新 shell(`cinux> ` 提示符、`pwd` 输出 `/`、能 `help`);**再点一次**,弹出第二个终端、第二个独立 shell。在 A 里敲 `ls`,B 里毫无反应——两个独立 shell 进程,各自走各自的管道。这是 033 做不到的(那时它们共享一个 shell),也是 034+035 通电 fork/exec 的最终回报。

```text
[INIT] ===== Milestone 035: Multi-Terminal =====     ← 这条里程碑在 init.cpp 打印
...
[GUI] Terminal 'Shell #1': shell spawned pid=2        ← 其余在 gui_init.cpp,前缀 [GUI]
[GUI] Shell child jumping to user mode: entry=<入口>  ← 入口地址依 ELF 而定
...
[GUI] Terminal 'Shell #2': shell spawned pid=3
```

看到 `Shell #1: shell spawned pid=2` 和 `Shell #2: shell spawned pid=3`——两个不同的 PID,就是两个独立的 shell 进程。(上面是示意串口,不是逐字实录:里程碑横幅由 `init.cpp` 用 `[INIT]` 打,终端相关日志由 `gui_init.cpp` 用 `[GUI]` 打,前缀不同因为它们在不同源文件;pid 按实际 fork 顺序排,入口地址随 ELF 变。)`

## 下一站

到 035b,Cinux 的 GUI/多任务弧画上句号。回头看整条路:从 MBR 引导(001)起步,进保护模式(002)、长模式(003),搭 mini kernel(005-007),再进 big kernel 的 GDT/IDT(010)、中断(011)、驱动(012-014)、内存管理(015-018)、进程与调度(019-021)、用户态与 syscall(022-023)、shell(024)、磁盘与文件系统(025-028),然后是图形(029-033)、进程原语(034)、通电与多终端(035)——一台从零搭起来的、有窗口、有图标、能开多个独立终端的小操作系统,跑在眼前。

这之后的事(更完善的 IPC、真正的阻塞 waitpid、USB 鼠标、网络……)是新的一章了,不在 milestone 035 之内。

## 参考

- Linux man-pages `fork(2)` / `execve(2)` / `waitpid(2)`:fork 生进程、execve 换映像、waitpid 收尸——035b 把这三者第一次端到端用起来。https://man7.org/linux/man-pages/man2/fork.2.html
- Linux man-pages `pipe(2)` / `read(2)` / `write(2)`:管道 + 标准流(fd0/fd1)语义,每个终端一对私有管道即此模型。https://man7.org/linux/man-pages/man2/pipe.2.html
- Linux man-pages `open(2)` / `fcntl(2)`:每个进程有自己的文件描述符表(fd0/fd1/...),035b 子进程新建私有 `FDTable`、用 `set(0/1)` 占据标准流即此模型的体现。https://man7.org/linux/man-pages/man2/open.2.html
