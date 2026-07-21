---
title: Lab 045 · POSIX 信号 验证
---

# Lab 045 · POSIX 信号 验证

> 对应 `document/book/14-process-advanced/045-signals.md`。验证档 **A 档**(能 sigaction 捕获)+ B(投递内部)。验证靠构建 + 测试 + grep + handler 往返。

## 目标

确认四件事:

1. 信号数据结构 + 投递三件套在(`signal_send`/`pick_deliverable`/`check_and_deliver`);
2. Custom handler 走 ISR 路径(`signal_setup_frame` 建栈帧),sigreturn 经 `int $0x80`;
3. 三处集成:PF→SIGSEGV(替代 042 的 exit_current)、exit→SIGCHLD、write→SIGPIPE;
4. run-kernel-test 从 044 的 763 涨到 783。

## 步骤

### 1. 构建 + 测试

```bash
git checkout 045_proc_signals 2>/dev/null || git checkout 045_*
cmake -B build -S . && cmake --build build -j$(nproc) > /tmp/b.log 2>&1; echo "build=$?"
cmake --build build --target run-kernel-test
```

### 2. 数据结构 + 投递

```bash
grep -rn 'signal_send\|signal_pick_deliverable\|signal_check_and_deliver\|signal_setup_frame' kernel/proc/signal.cpp
grep -rn 'sys_kill\|sys_rt_sigaction\|sys_rt_sigprocmask' kernel/syscall/sys_signal.hpp
```

**思考**:为什么 Custom handler 不走 syscall 路径投递?——见章节:`syscall.S` 帧精简(无 R12-R15),sigreturn 没法完整恢复。Custom 走 ISR 返回路径建完整 SignalFrame,sigreturn 经 `int $0x80` 收完整 InterruptFrame。syscall 路径只投 Default/Ignore,Custom 留 pending 等下次 IRQ0。

### 3. PF→SIGSEGV(和 042 对照)

```bash
grep -n 'SIGSEGV\|signal_send' kernel/arch/x86_64/exception_handlers.cpp
```

对比 042:那里 user-fault no-VMA 是 `exit_current` 直接杀;045 改成 `signal_send(SIGSEGV)+return`,ISR 投递——能被 handler 捕获,或默认终止。**这是 042→045 的行为演进。**

### 4.(A 档)handler 往返 + sigreturn trampoline

```bash
grep -n 'cd 80\|int .0x80\|trampoline' kernel/proc/signal.cpp
```

去看 signal.cpp 里 sigreturn trampoline 的构造(栈上 `cd 80`+nop,handler 返回地址指它)。内核测试里 `signal_setup_frame` 构造 + sigreturn 恢复两条单测覆盖往返。**思考**:为什么这个 trampoline 是"登记好的债"?——见章节 GOTCHA#10:依赖栈可执行,NXE 未启用故可行;F9(056)启用 NXE 后栈不可执行,trampoline 失效,须迁 vdso。

## 验收清单

- [ ] 构建 `build=0`,run-kernel-test ~783。
- [ ] `signal_send`/`pick_deliverable`/`check_and_deliver`/`setup_frame` + kill/sigaction/sigprocmask 都在。
- [ ] PF→SIGSEGV 替代了 042 的 exit_current;exit→SIGCHLD、write→SIGPIPE 在。
- [ ] 能说清「Custom 为何走中断路径」「sigreturn trampoline 为何是债」。

## 别做这些

- **别**在 syscall 路径投递 Custom——帧不全,sigreturn 恢复不全。
- **别**忘了 `signal_check_and_deliver_isr` 要判 `frame->cs&0x03`——否则 kernel-test(ring0)被误投递,测试崩。
- **记着**:F9 启用 NXE 后,sigreturn trampoline 必须迁 vdso(栈不可执行)。
