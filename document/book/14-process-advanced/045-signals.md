---
title: 045 · POSIX 信号
---

# 045 · POSIX 信号:让用户程序能捕获 SIGSEGV

> 进 F3 进程弧。这一章从零建 **POSIX 信号**(核心 22 个):投递 / 处理 / 屏蔽 + `kill`/`sigaction`/`sigprocmask`/`sigreturn` + 自定义 handler 往返 + 三处集成(PF→SIGSEGV、exit→SIGCHLD、write→SIGPIPE)。A 档:用户程序能 `sigaction` 注册 handler 捕获信号了——比如 SIGSEGV 不再是"直接杀",可以接住、处理、继续。
>
> 这一章也和 042 耦合:042 的 PF 硬门控对野指针直接 `exit_current`,045 把它改成投递 SIGSEGV——能被捕获,或默认终止。

## 这章咱们要点亮什么

1. **信号数据结构**:`Signal` 枚举(22 个)、`SigSet` 位掩码、`SigAction` + `Task` 的信号字段 + fork 继承(清 pending)。
2. **投递三件套**:`signal_send` 投递、`signal_pick_deliverable` 选可投递的、`signal_check_and_deliver` 在返回路径投递。
3. **自定义 handler 走中断路径**(关键决策):不改 `syscall.S`,而是在 ISR 返回路径建信号栈帧。
4. **sigreturn 栈注入 trampoline**:handler 的返回地址指向栈上的 `int $0x80`,靠它 trap 回内核恢复上下文。
5. **三处集成**:PF→SIGSEGV、exit→SIGCHLD、write→SIGPIPE。

## 为什么现在需要它

042 给了 PF 硬门控——野指针、非法访问直接 `exit_current` 杀进程。但这太硬:用户程序**接不住**——没法像 Linux 那样 `sigaction(SIGSEGV, ...)` 捕获、做点恢复、继续跑。也没有 SIGCHLD(父进程不知道子进程死了)、没有 SIGPIPE(write 一个读端关掉的 pipe 该被通知)。信号是 Unix 进程模型的标配,缺了它,后面的 shell、job control、musl 都铺不下去。

## 数据结构 + 投递

`kernel/proc/signal.{hpp,cpp}`:

```cpp
// signal.cpp:116 —— 投递一个信号给 target(置 pending 位)
int signal_send(Task* target, Signal sig);
// signal.cpp:135 —— 从 pending 里挑一个"可投递"的(未被 block 的)
int signal_pick_deliverable(Task* task, bool allow_custom);
// signal.cpp:177 —— 在 syscall 返回路径检查并投递(只 Default/Ignore)
void signal_check_and_deliver();
```

`Signal` 是 22 个核心信号的枚举,`SigSet` 是 64 位掩码(每位一个信号),`SigAction` 记一个信号的处理方式(handler 地址 + mask + flags)。`Task` 加了 `sig_pending`/`sig_blocked`/`sig_actions[]` 一组字段;fork 时子进程**清 pending**(不继承父的待处理信号)。

投递分两条路:**Default/Ignore** 在 syscall 返回路径就能投(`signal_check_and_deliver`,因为不需要建栈帧);**Custom handler** 不行——见下。

## 关键决策:Custom handler 走中断路径,不改 syscall.S

这是这一弧最精巧的地方。自定义 handler 要"切到用户态 handler 跑,跑完再 sigreturn 恢复原来的上下文"。难点是 **`syscall.S` 保存的帧太精简**——只有 user_rsp/rip/rflags + 6 参数 + rbx/rbp,**没有 R12-R15**,sigreturn 没法完整恢复用户上下文。

两个选择:把 `syscall.S` 改成完整帧(高风险,影响所有 syscall);或者让 sigreturn 走 **`int $0x80` 软中断**,它经 IDT 进来,收的是完整 `InterruptFrame`。这一弧选后者:

- ISR 宏在 `call handler` 之后调 `signal_check_and_deliver_isr(frame)`(`signal.cpp`);
- Custom 信号时,`signal_setup_frame`(`signal.cpp:204`)**在用户栈上构造 SignalFrame**(保存完整上下文)+ 改 `frame` 让 iret 跳到 handler;
- handler 跑完 `ret` 时,落到栈上的 `int $0x80` trampoline(见下),trap 回内核;
- sigreturn 经 IDT vector 0x80 的 trap gate(DPL=3,用户能触发)进入,用 SignalFrame 里的完整上下文恢复。

syscall 路径(`signal_check_and_deliver`)只投递 Default/Ignore;Custom 留 pending,等下次 IRQ0(时钟中断,很频繁)在 ISR 路径投递——延迟可忽略。

## sigreturn trampoline:栈注入(GOTCHA #10,NXE 耦合)

handler 的返回地址要指向"一段能让内核接管、恢复上下文的代码"。这一弧的做法是**把 `int $0x80`(机器码 `cd 80` + nop,填 8 字节槽)写到用户栈上**,让 handler 的返回地址指它:

```cpp
// signal.cpp:38-40 —— sigreturn trampoline 写到用户栈;handler 的返回地址指这,
// ret 落到 int $0x80,trap 回内核走 sigreturn 恢复完整上下文
```

这里埋着一颗**和 F9 耦合的雷**(GOTCHA #10):这依赖**栈可执行**——handler `ret` 到栈上的 `cd 80`,得能执行。此刻 EFER.NXE **没启用**(F9 才做),栈是可执行的,所以能用。**等 F9 启用 NXE,栈变成不可执行,这个 trampoline 就失效了**,必须迁到 vdso 或独立可执行页。

> 教训:栈注入代码是个"现在能用、将来会断"的方案。明知 F9 要开 NXE,这里就是一笔登记好的债——到 056(安全弧启用 NXE)时,要把 sigreturn trampoline 挪到 vdso。patch-replay 忠实还原了 CinuxOS 当时的做法(它也是先这么干、F9 再迁的)。

## 只对用户态投递

`signal_check_and_deliver_isr` 严判 `frame->cs & 0x03`(是不是 ring3 触发的)。因为内核测试全是 ring0(中断点 cs=kernel),要是也投递,会误把测试设的 pending 栈 task 投递掉、`exit_current` 切走 → 测试崩。只有真实用户态(ring3)中断点才投递。

## 三处集成

- **PF→SIGSEGV**(`exception_handlers.cpp:282`):042 的 user-fault no-VMA 不再 `exit_current`,改成 `signal_send(SIGSEGV) + return`——ISR 的 signal check 会投递它,能被 handler 捕获,或默认终止(SIGSEGV default = terminate)。kernel-test 是 ring0,不走这条(`err&0x04` 守卫)。
- **exit→SIGCHLD**:`sys_exit` 设 Dead 前给父进程 `signal_send(SIGCHLD)`。SIGCHLD default = Ignore(`kIgnore` no-op),不杀父。
- **write→SIGPIPE**:`sys_write` write 失败且 `errno==EPIPE` 时,给自己 `signal_send(SIGPIPE)`。

## 遗留(明说)

- **waitpid 仍 non-blocking**(轮询)。阻塞 + SIGCHLD 唤醒需要 `wait_queue`,留 F3-M2(futex 同类)。
- **进程组 kill**(`pid<0`)留 F3-M3。
- **sigreturn trampoline 迁 vdso**:等 F9 NXE(056)。
- 实时信号(33-64)、sigaltstack 真启用、SA_RESTART 真语义、信号嵌套——留后续。

## 验证

```bash
# 数据结构 + 投递
grep -rn 'signal_send\|signal_pick_deliverable\|signal_check_and_deliver\|signal_setup_frame' kernel/proc/signal.cpp
# syscall
grep -rn 'sys_kill\|sys_rt_sigaction\|sys_rt_sigprocmask' kernel/syscall/sys_signal.hpp
# PF→SIGSEGV(替代 042 的 exit_current)
grep -n 'SIGSEGV\|signal_send' kernel/arch/x86_64/exception_handlers.cpp
```

构建 + 内核测试(这一弧 run-kernel-test 从 044 的 763 涨到 783,+20):

```bash
cmake --build build -j$(nproc) > /tmp/b.log 2>&1; echo "build=$?"
cmake --build build --target run-kernel-test
```

A 档的端到端是 Custom handler 往返:`sigaction(SIGSEGV, my_handler)` → 故意解引用空指针 → handler 跑 → sigreturn 恢复 → 程序继续。libc wrapper 已就绪,用户程序真触发要等 F10 musl;现在内核测试里的 `signal_setup_frame` 构造 + sigreturn 恢复两条单测覆盖了往返逻辑。

## 小结与下一站

信号地基立起来了:能投递、能屏蔽、能注册 handler、能往返恢复,还接上了 PF/exit/write 三处。waitpid 阻塞、进程组 kill 这些留后面 F3 的 M2/M3。

下一站 **046** 继续进程弧:`clone` + `futex` + TLS——真正的线程。
