---
title: 04 · 收尾:验证 + 没做的 + 下一站 + 参考
---

# 收尾:验证 + 没做的 + 下一站 + 参考

## 验证

**第一层:host 单元测试。** 多终端、fork/exec、pipe 的纯逻辑(host 镜像):

```bash
ctest --test-dir build -R "multi_terminal|fork_exec|pipe" --output-on-failure
```

**第二层:QEMU kernel 测试。** 真跑内核、走真 fork/exec/#PF:

```bash
cmake --build build --target run-big-kernel-test
```

覆盖 `run_fork_exec_tests`、`run_multi_terminal_tests` 等。这一层对 035 尤其重要——CoW 是否真在 #PF 里工作、子进程是否真返回 0、GS MSR 是否跨切换保持,都得在真 CPU + 真 syscall 上验。

**第三层:端到端。** 035 不只是内核基础设施——同一个 tag 里,GUI 侧已经把它用起来了:点 shell 图标时 `fork` + `execve("/bin/sh")`,把一个独立 shell 跑进用户态。tag-035 当时这套逻辑在内核侧的 `kernel/gui/gui_init.cpp`;后来 GUI 整体外置到用户态 host,对应代码现在在 [user/cinux_gui_host/main.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/user/cinux_gui_host/main.cpp)(点 Shell 图标 → `fork` + `execve("/bin/sh")`,见其中的 `launch_shell`/shell spawn 路径)。

```bash
cmake --build build --target run
```

点 shell 图标,你会看到 fork+execve 出一个独立 shell、进用户态跑起来、能执行命令——这就是 035 这几堵墙被推倒后,内核侧真正能端到端跑的证据。至于「开多个终端、各自独立 shell」的完整体验(私有管道、私有 fd 表),是 035b 的主题,下一章展开。

## 下一站

到 035,内核侧的 fork/exec 彻底通了:子进程会返回 0、CoW 在写时真的复制、syscall 跨切换正常、ELF 每页填对。地基夯实了——而且同一个 tag 里,这套能力就已经被 GUI 侧用了起来(fork+execve 出独立 shell;tag-035 在内核 GUI,后迁用户态 host)。

下一章(035b),我们专门看 GUI 那半:怎么把「点图标 → fork+execve → 独立 shell」做成**多终端**——每个终端一对私有管道、一张私有 fd 表,多个 shell 进程互不串扰。那是整条 GUI/多任务弧的高潮。

## 参考

- Intel SDM / AMD64 APM:`swapgs` 指令、`MSR_GS_BASE`(0xC0000101)/`MSR_KERNEL_GS_BASE`(0xC0000102)、SYSCALL/SYSRET、TSS 的 IST(Interrupt Stack Table)、页表 available bits。这些是 GS MSR、IST、CoW 那几堵墙的硬件依据。
- Linux man-pages `fork(2)`:fork 在父进程返回子 PID、在子进程返回 0——035 用 `fork_child_trampoline` 终于实现。https://man7.org/linux/man-pages/man2/fork.2.html
- OSDev Wiki — Page Fault:error code 的 bit0(present)/bit1(write)/bit2(user)含义,`handle_pf` 据此分流 demand-paging / CoW / 致命。https://wiki.osdev.org/Exceptions#Page_Fault
- OSDev Wiki — ELF:`PT_LOAD` 段、`p_filesz`/`p_memsz`,execve 页内偏移 bug 的背景。https://wiki.osdev.org/ELF
