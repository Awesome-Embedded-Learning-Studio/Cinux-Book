---
title: Lab 035b · 多终端:每个终端一个独立 shell
---

# Lab 035b · 多终端:每个终端一个独立 shell

> 对应主书 [035b · 多终端](../../book/10-multitasking/035b-multi-terminal.md)。本 lab 用 035 通电后的 fork/execve,把「点图标开终端」从「共享一个 shell」改造成「每个终端一个独立 shell」。给任务、约束、验证,不贴完整答案。

## 实验目标

点 shell 图标 → `fork` + `execve("/bin/sh")` 生出一个独立 shell 进程;开多个终端,各自的 shell 互不串扰;关终端时把它的 shell 收尸。

## 前置条件

- 完成 035(fork/exec 通电:子进程返回 0、CoW 接 #PF、GS MSR、execve 页偏移、栈 guard page)。
- 完成 031b(Pipe + PipeReadOps/PipeWriteOps 伪装成 Inode、fd0/fd1 绑管道)和 033(桌面图标、点图标开窗口)。
- `jump_to_usermode`、per-CPU `update_syscall_stack` 可用。

## 任务分解

### 任务 1:每个终端一对私有 pipe

把 033 的「全局 g_stdin_pipe/g_stdout_pipe」改成「每终端 new 自己一对」。

- 在 `create_shell_terminal` 里:`new Pipe()` × 2(stdin/stdout),各包 `PipeReadOps`/`PipeWriteOps` + `Inode`(type=Regular)。
- `term->set_stdin_pipe/stdout_pipe` 绑这一对(内核侧直接访问)。
- **约束**:每对 pipe 是这个终端私有的,不能共享全局对象。

### 任务 2:fork + 子进程建私有地址空间与 fd 表

- `fork()`:父侧 `term->set_shell_pid(child_pid)`;子侧(child_pid==0)走任务 3。
- 子侧先 `cli`;`task->addr_space = new AddressSpace()`(父 gui_worker 是无地址空间内核线程);`task->fd_table = new FDTable()`。
- **关键**:`FDTable::set(0, File(stdin_read_inode, RDONLY))`、`set(1, File(stdout_write_inode, WRONLY))`——用 `set` 占 fd0/fd1,不是 `alloc`(alloc 跳过 0/1/2)。
- 思考:为什么这里 035 的 CoW 页表复制用不上?(父没有用户地址空间,子从零建。)

### 任务 3:子进程 execve + 进用户态

- `execve("/bin/sh", argv, envp)`;失败则 `Scheduler::exit_current()`。
- 建用户栈(`USER_STACK_PAGES` 页,FLAG_PRESENT|WRITABLE|USER)。
- `task->addr_space->activate()`(切到子进程页表)。
- `g_per_cpu.update_syscall_stack(task->kernel_stack_top)`(让 syscall_entry 加载子进程内核栈)。
- `jump_to_usermode(entry, USER_STACK_TOP - USER_ABI_RSP_OFFSET, 0)`。
- **约束**:`jump_to_usermode` 前**不要** `sti`(SYSRETQ 用 R11 恢复 RFLAGS、原子开中断;提前 sti 会让 PIT 在子 CR3 上炸 framebuffer)。

### 任务 4:延迟到 gui_worker(别在中断里 fork)

- fork/execve 不能在 PIT 滴答回调(中断上下文)里跑。
- 用一个 `std::atomic<IconAction> g_pending_action`:点图标(中断)投递、`gui_worker` 内核线程排空后调 `create_shell_terminal`。
- **约束**:fork/execve/address-space 操作只在 `gui_worker` 线程上下文做。

### 任务 5:Terminal 持 shell_pid + 析构收尸

- `Terminal` 加 `shell_pid_`(初值 0);`set_shell_pid`/`shell_pid`。
- 析构:`if (shell_pid_ > 0)` 有界循环(上限 1000)调 `waitpid(shell_pid_, &status)`,Ok/NoChildren/NotFound 跳出,最后 `shell_pid_ = 0`。
- **约束**:034 的 waitpid 是非阻塞的,这里用有界自旋凑成「近似阻塞」收尸,防 zombie。

## 接口约束

- 每终端独立 `Pipe` 对(不共享全局)。
- 子进程私有 `FDTable`,fd0/fd1 显式 `set`(非 alloc)。
- fork/execve 只在 `gui_worker` 线程上下文跑,不在 PIT 中断里。
- `jump_to_usermode` 前不 sti。

## 验证步骤

**host 单测**:

```bash
ctest --test-dir build -R multi_terminal --output-on-failure
```

重点:`test_multi_term_two_terminals_independent_pipes` 一类——两终端管道互不串扰。

**QEMU 机内测试**:

```bash
cmake --build build --target run-big-kernel-test
```

`run_multi_terminal_tests()` 通过。

**端到端(验收)**:

```bash
cmake --build build --target run
```

点 shell 图标开终端 #1、再点开终端 #2;在 #1 敲 `ls`,#2 不受影响。串口见 `Shell #1: shell spawned pid=2`、`Shell #2: shell spawned pid=3`(不同 PID = 独立 shell)。关一个终端,见 `[TERM] Reaped shell pid=...`。

## 常见故障

- **两个终端内容互串**:fd0/fd1 绑成了全局 pipe,或没建私有 `FDTable`(子进程继承了父的 fd 表)。检查任务 1/2:每终端 new 自己的 Pipe、子进程 new 自己的 FDTable。
- **`sys_read(0)`/`sys_write(1)` 没走自己的管道**:用了 `FDTable::alloc` 而非 `set(0/1)`(alloc 跳过标准流)。改成 `set`。
- **一开第二个终端就崩/花屏**:`jump_to_usermode` 前多 `sti` 了,或没 `activate()` 子进程地址空间、没 `update_syscall_stack`。看任务 3 的约束。
- **fork/execve 在滴答里跑导致调度器状态错乱**:没延迟到 gui_worker。看任务 4。
- **关终端后留 zombie / PID 泄漏**:析构没 waitpid 收尸,或没用 `shell_pid_` 跟踪。看任务 5。

## 通过标准

- [ ] 点 shell 图标 fork+execve 出独立 shell,进用户态能跑命令。
- [ ] 开两个终端:各自的 shell(不同 PID)、各自的 pipe,在一个里敲命令另一个无反应。
- [ ] 子进程私有 `FDTable`,fd0/fd1 绑自己的 pipe(用 `set` 非 `alloc`)。
- [ ] fork/execve 在 `gui_worker` 线程上下文执行(经 `g_pending_action` 投递),不在 PIT 中断里。
- [ ] `jump_to_usermode` 前未 sti。
- [ ] 关终端时 `waitpid` 收尸自己的 shell,无 zombie(`[TERM] Reaped shell pid=...`)。
- [ ] host `-R multi_terminal` 与 `run-big-kernel-test` 通过。
