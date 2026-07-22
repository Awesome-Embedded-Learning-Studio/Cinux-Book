---
title: 045 · POSIX 信号
---

# 045 · 让用户程序能接住 SIGSEGV:POSIX 信号

> 你的程序解引用了一个空指针。在 042 之前,内核直接把它杀了(`exit_current`)——程序没机会做任何事。可在 Unix 里,程序能 `sigaction(SIGSEGV, my_handler)` **接住**这个信号,做点恢复、记个日志、甚至继续跑。信号是 Unix 进程模型的标配:缺了它,shell 的任务控制、子进程退出通知、管道断裂通知全都铺不下去。这一章从零建 POSIX 信号——投递、屏蔽、自定义 handler、往返恢复,再把它接到缺页、子进程退出、管道断裂三个地方。

## 信号是什么

一个信号就是一个"异步通知":内核(或别的进程)告诉某进程"发生了某件事"(内存非法访问、子进程死了、管道断了……)。每个信号有个编号和**默认动作**(大部分是终止进程,有的忽略),进程也可以 `sigaction` 注册一个**自定义 handler**,信号来时切到 handler 跑、跑完再回到被打断的地方。

内核侧的数据结构很简单(`kernel/proc/signal.{hpp,cpp}`):

- `Signal` 枚举(22 个核心信号)、`SigSet` 位掩码(64 位,每位一个信号);
- 每个 `Task` 带 `sig_pending`(待处理的信号位图)、`sig_blocked`(被屏蔽的)、`sig_actions[]`(每个信号的处理方式);
- 投递:`signal_send` 置位 pending;选可投递的:`signal_pick_deliverable`(pending 里、没被 block 的);在返回路径检查并投:`signal_check_and_deliver`。

## 难点:自定义 handler 怎么"切过去再切回来"

默认动作(终止 / 忽略)好办——投递时直接做。自定义 handler 难:要"保存当前上下文 → 切到 handler 跑 → 跑完 sigreturn 恢复原来的上下文"。难点在 **sigreturn 要能完整恢复用户态全部寄存器**。

这一章的**关键决策**:自定义 handler **不走 syscall 路径,走中断路径**。原因是 `syscall.S` 保存的帧太精简(只有部分寄存器,**没有 R12-R15**),sigreturn 没法完整恢复。而走 `int $0x80` 软中断,经 IDT 进来,收的是**完整的中断帧**。所以:

- 中断处理完、要返回用户态之前,检查有没有待投递的自定义信号(`signal_check_and_deliver_isr`);
- 有的话,`signal_setup_frame` 在**用户栈上构造一个信号帧**(保存完整上下文),并把返回地址改成 handler;
- 中断返回(iret)就跳进了 handler;
- handler 跑完 `ret`,落到栈上预先放好的 `int $0x80`(机器码 `cd 80`),trap 回内核;
- sigreturn 经 IDT 的 0x80 号 trap gate(特权级 3,用户能触发)进入,用信号帧里保存的完整上下文恢复,程序回到原本被打断的地方。

syscall 路径只投递默认 / 忽略动作;自定义的留 pending,等下次时钟中断(IRQ0,很频繁)在中断路径投递——延迟可忽略。

### 一个埋着的雷:栈上的 `int $0x80` trampoline

handler 的返回地址指向**栈上**写的一段 `int $0x80`(加 nop 填满 8 字节槽)。这依赖**栈可执行**——`ret` 跳到栈上的 `cd 80`,得能执行它。此刻 NXE(不可执行使能)**还没开**(安全弧才做),栈是可执行的,所以能用。**等安全弧开了 NXE,栈变成不可执行,这个 trampoline 就失效**,得挪到独立可执行页(vdso)。这是一笔登记好的债——先把能力做出来,外壳后面再换。

> 还有条守则:中断路径投递信号时,严判 `frame->cs & 0x03`(是不是用户态触发的)。内核测试全是内核态,要是也投递,会把测试设的待处理状态搞乱、甚至切走测试任务的栈导致崩溃。只有真实用户态的中断点才投。

## 接到三个地方

信号框架立好,把它接到三个会产生信号的点:

- **缺页 → SIGSEGV**:042 那里,用户态访问没 VMA 的区域原来是 `exit_current` 直接杀;现在改成 `signal_send(SIGSEGV)` + 返回。中断路径的信号检查会投递它——于是它能被自定义 handler 接住,或按默认动作(终止)。**042 的"直接杀"升级成了"投个能被接住的信号"。**
- **子进程退出 → SIGCHLD**:子进程 `exit` 之前,给父进程投 SIGCHLD(默认动作是忽略,不杀父)。父进程因此能知道"自己的子进程死了"。
- **管道断裂 → SIGPIPE**:`write` 一个读端已关的管道失败时,给自己投 SIGPIPE。

## 验证

```bash
# 投递三件套 + syscall
grep -rn 'signal_send\|signal_pick_deliverable\|signal_check_and_deliver\|signal_setup_frame' kernel/proc/signal.cpp
grep -rn 'sys_kill\|sys_rt_sigaction\|sys_rt_sigprocmask' kernel/syscall/sys_signal.hpp
# 缺页改投 SIGSEGV(对照 042 的 exit_current)
grep -n 'SIGSEGV\|signal_send' kernel/arch/x86_64/exception_handlers.cpp
```

构建:

```bash
cmake -B build -S . && cmake --build build -j$(nproc) > /tmp/b.log 2>&1; echo "build=$?"
```

端到端是自定义 handler 往返:`sigaction(SIGSEGV, my_handler)` → 故意解引用空指针 → handler 跑 → sigreturn 恢复 → 程序继续。用户程序真触发要等后面 musl 的 C 库;内核测试里"信号帧构造 + sigreturn 恢复"两条单测覆盖了往返逻辑。想体会这个往返有多实在:handler 跑完能回到原本被打断的下一条指令,程序像什么都没发生一样继续——这就是信号的精髓。
