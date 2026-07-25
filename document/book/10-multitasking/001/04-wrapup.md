---
title: 04 · 收尾:验证 + 没做的 + 下一站 + 参考
---

# 收尾:验证 + 没做的 + 下一站 + 参考

## 验证

和前面几章一样,034 分三层验证:纯逻辑 host 单测、QEMU 机内测试、端到端跑内核。

**第一层:host 单元测试。** PID 分配器、TCB 新字段、`FLAG_COW` 位运算、CoW PTE 状态机、syscall 号、`ExecveResult` 的 errno 映射、ELF 结构尺寸与校验——这些不碰真硬件,在 host 上 `-DCINUX_HOST_TEST` 编、链上 `pid.cpp` + `elf_types.cpp` 跑:

```bash
ctest --test-dir build -R fork_exec --output-on-failure
```

注意它**不**跑真 fork(那需要调度循环和真页表),只验「给我这个 PTE / 这个 PID 状态,算出来的结果对不对」——比如 `FLAG_COW` 标记转换、PID 分配的复用/耗尽、ELF 头校验的各种坏case。一次跑全部 host 测试:

```bash
cmake --build build --target test_host
```

**第二层:QEMU kernel 测试。** 真跑内核代码、走真 syscall 分发的机内测,在 `main_test.cpp` 里 scheduler 和 `syscall_init()` 之后注册了 `run_fork_exec_tests()`:

```bash
cmake --build build --target run-big-kernel-test
```

它验:getpid/getppid 直调和经 `syscall_dispatch` 两条路返回值一致(测试里临时装一个 `Task{pid=42,ppid=1}` 让 syscall 有 current 可读)、PID 分配器机内 smoke、`FLAG_COW` 位定义、CoW PTE 的「标记为只读+COW → 写后解析成私有可写页」转换、sys_fork 分发路径可达、ExecveResult 的 errno 数值、ELF64 头校验(合法头过、坏魔数/坏类别/坏机型/无 program header 各返回对应错误)。

**第三层:端到端。** 这一层 034 比较特殊——因为前面「调试现场」讲的那个缺口,「fork 出子进程、它 execve、父进程 waitpid 收到退出码」这条**完整**链路在 034 还没法漂亮地演示(子进程的返回值闭环没接上)。所以这一层更像是「看日志确认子系统起来了」:

```bash
cmake --build build --target run
```

留意串口里 fork/execve/waitpid 各自的 `[PROC] fork: created child pid=...`、`[EXECVE] loaded ... entry=...`、`[WAITPID] reaped child pid=...` 这几行。能在日志里看到它们被走到,说明五个 syscall 已经接进分发、底层设施各司其职。完整的「spawn 一个独立程序并等它结束」演示,要等下一章把 fork 的控制流闭环补上,才真正漂亮。

## 下一站

到 034,我们有了 fork、execve、waitpid 三个原语和 CoW 页表——进程能生、能换、能收。但有两件事还没收口:一是 fork 让子进程「返回 0」的控制流闭环,二是 waitpid 还是非阻塞的。这些是 034 留下的、源码里看得见的待办。

有了这些原语,一件自然而然的事变得可能:**给每个终端 spawn 它自己独立的 shell 进程**。033 的桌面只能有一个终端里转一个 shell;有了 fork+execve,我们可以 fork 出子进程、让它 execve 成 shell,父子用管道通信——于是桌面能同时开**多个**终端,每个跑自己的 shell,互不干扰。怎么把这些原语拼成「多终端桌面」,并顺手把 fork 那些还没填的坑填上,是下一章的事。

## 参考

- Linux man-pages — `fork(2)`:fork 返回语义(父进程得子 PID、子进程得 0、子进程是父进程的拷贝)。https://man7.org/linux/man-pages/man2/fork.2.html
- Linux man-pages — `execve(2)`:用新程序替换当前进程映像(新栈/堆/数据段)、`argv`/`envp` 语义。https://man7.org/linux/man-pages/man2/execve.2.html
- Linux man-pages — `waitpid(2)`:收尸 zombie 子进程、`ECHILD`、`status` 收集。https://man7.org/linux/man-pages/man2/waitpid.2.html
- OSDev Wiki — ELF:程序头、`PT_LOAD`、`p_filesz`/`p_memsz` 与 BSS 的关系。https://wiki.osdev.org/ELF
- OSDev Wiki — Page Tables:x86-64 页表项位域概览。其中 PTE 的 bits 9-11 是 CPU 不解释的「可用/忽略」位(这是 Intel SDM 与 AMD64 APM 定义的标准架构事实),`FLAG_COW` 正是复用其中的 bit 9。https://wiki.osdev.org/Page_Tables
