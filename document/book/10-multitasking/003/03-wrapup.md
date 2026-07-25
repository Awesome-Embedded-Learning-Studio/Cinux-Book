---
title: 03 · 收尾:验证 + 没做的 + 下一站 + 参考
---

# 收尾:验证 + 没做的 + 下一站 + 参考

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
