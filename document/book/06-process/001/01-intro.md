---
title: 01 · 导引:点亮什么、为什么、设计图
---

# 导引:点亮什么、为什么、设计图

## 这一章我们要点亮什么

核心是一件:**让内核从「一条 main 流」变成「多条可切换的内核线程」**。

具体说,019 交付四块:

- **任务抽象**:`TaskState`(运行/就绪/阻塞/死亡四种生命周期)、`CpuContext`(一段 64 字节的寄存器快照)、`Task`(任务控制块 TCB,装着上下文、状态、栈、名字)、`TaskBuilder`(流式构造器)。一个 `Task` 就是「一条可以被挂起、又被恢复的执行流」的全部载体。
- **上下文切换**:[context_switch.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/context_switch.S)——一段汇编,保存当前任务的寄存器、恢复下一个任务的寄存器、换栈、一跳。这一段是整章的灵魂,也是后面所有「并发」的物理基础。
- **调度器骨架**:[scheduler.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/scheduler.cpp)——`RoundRobin` 轮转队列 + `Scheduler` 静态门面(`init`/`add_task`/`yield`/`exit_current`/`run_first`)。负责「下一个该轮到谁」。
- **higher-half 收口**:把大内核从恒等映射地址扶正回它链接的 higher-half 地址,顺带修一个缺页处理的小毛病。这块不是「新功能」,但它是「进程隔离能成立」的前提。

合起来,这一章给了内核「同时持有多个活动、并在它们之间切换」的能力。但要把期望放正:019 是**协作式**多任务——线程不主动 `yield()`,它就独占 CPU 到天荒地老;切换只由手动 `yield` 触发,没有抢占、没有优先级生效、没有用户态。这里有个容易误会的地方得说清:时钟中断这时候**其实在跑**(PIT 从 011 章就初始化了,`main` 里 `sti` 之后,IRQ0 每个 tick 都会进 `pit_irq0_handler`),只是那个 handler 目前只递增一个 tick 计数、还没接到调度器,所以不会强行打断线程。把调度器挂到时钟中断上做成抢占,是 020 的事。019 只回答最基础的一问:**「怎么把 CPU 从一条执行流手里拿过来、递给另一条,而且两条都还活着。」**

## 为什么现在需要它

先回答一个看代码时一定会冒出来的疑问:`CpuContext` 里只存了 8 个值——6 个 callee-saved(`r15/r14/r13/r12/rbp/rbx`)再加上 `rsp` 和 `rip`。x86-64 有 16 个通用寄存器,那其余的 caller-saved(`rax`/`rcx`/`rdx`/`rsi`/`rdi`/`r8`–`r11`)为什么不全存?

答案是 **System V AMD64 调用约定**。它把寄存器分成两类:

- **caller-saved(调用方保存)**:`rax rcx rdx rsi rdi r8 r9 r10 r11`。约定说:这些寄存器在函数调用过后**不保证**还是原值,谁要用谁自己存。所以编译器在调用别的函数之前,如果这些寄存器里有还要用的值,会**主动**把它们存到栈上;调用完再取回来。
- **callee-saved(被调方保存)**:`rbx rbp r12 r13 r14 r15`。约定说:这些寄存器在函数调用过后**保证**还是原值——谁改了谁负责恢复。

而我们的上下文切换,永远发生在「函数调用边界」上:`yield()` 调 `context_switch()`,`context_switch()` 又调回去。在 `yield` 调进 `context_switch` 的那一刻,按约定,caller-saved 寄存器里的值本来就是「不保证存活」的——编译器要么已经把它们存到 `yield` 的栈帧上了,要么根本不在乎。所以我们**根本不需要**替调用方保存它们。我们只需要保存 callee-saved 这 6 个(因为它们「承诺」跨调用不变,我们必须把当前值藏好,等恢复时还回去),再加上定义「执行流此刻在哪」的两个:`rsp`(栈顶,决定这条流的调用链在哪)和 `rip`(下一条指令,决定这条流接下来干什么)。

这就是 `CpuContext` 只有 64 字节的全部理由。存多了是浪费,存少了会破坏调用约定、把调用方的 callee-saved 寄存器改花。xv6 的 `swtch()` 用的是同一招——只存 callee-saved 那一组,换栈,然后 `ret`。这不是巧合,而是「在函数边界做切换」这个约束下,唯一省事又正确的做法。

至于「为什么先做协作式,再做抢占式」——因为抢占式(靠时钟中断强行打断)要求一件事:中断能在**任意**指令处把 CPU 接管走。那意味着中断现场(`InterruptFrame`)里必须能完整重建任意时刻的执行流,而不仅仅是函数边界。019 先把「函数边界切换」这条最干净的路走通,验证 `Task` + `context_switch` + `RoundRobin` 这套骨架是对的;020 再把调度器接到那个**已经在跑**的时钟中断上(让 `irq0` handler 在固定节拍调用调度、强制切走当前线程),`yield` 就不再需要线程主动调了。一步一步来,每步只加一个变量。

## 设计图

先看 `CpuContext` 的内存布局——它是 `context_switch.S` 和 `TaskBuilder` 之间的契约,偏移必须严丝合缝:

```text
   struct CpuContext (alignas 16, 共 64 字节)
   ┌──────────────────────────────────────┐
   │ offset  0:  r15   callee-saved        │
   │ offset  8:  r14   callee-saved        │
   │ offset 16:  r13   callee-saved        │
   │ offset 24:  r12   callee-saved        │
   │ offset 32:  rbp   callee-saved (帧指针)│
   │ offset 40:  rbx   callee-saved        │
   │ offset 48:  rsp   ← 栈顶: 执行流的"调用链"│
   │ offset 56:  rip   ← 下一条指令: 接着干什么 │
   └──────────────────────────────────────┘
        process.hpp 用 8 条 static_assert 锁死这些偏移
        context_switch.S 用同样的数字 (0/8/.../56) 读写
```

再看 `context_switch` 干了什么。它是一个「不对称」的函数:进去时在 A 的栈上,出来时在 B 的栈上,而且「出来」可能发生在很久以后:

```text
   context_switch(from=A.ctx, to=B.ctx)            调用者: yield() / run_first()

   ① 把当前 CPU 的 callee-saved 存进 from(A.ctx):
        A.ctx.{r15..rbx} = 当前寄存器
        A.ctx.rsp        = 当前 rsp
        A.ctx.rip        = .restore 标号地址   ← 关键: 记下"回来时从这儿继续"

   ② 从 to(B.ctx) 把寄存器恢复出来:
        当前寄存器       = B.ctx.{r15..rbx}
        当前 rsp         = B.ctx.rsp           ← 换栈! 这一刻执行流切到 B

   ③ jmp *B.ctx.rip                                  ← 不是 call, 直接跳

        ┌─ B 是"全新的"任务(第一次被切到): rip=线程入口, rsp 指向干净栈(顶上压着 exit_current)
        │     → 跳进线程函数, 从头跑; 函数 return 时弹栈 → exit_current 干净退场
        │
        └─ B 是"被打断过"的任务(之前切走时 rip 被存成了 .restore):
              → 跳到 .restore → ret → 回到当初调用 context_switch 的地方
                (yield / run_first), 但此刻跑在 B 自己的栈上, 可能是"很久以后"
```

最后是协作式调度的实际节奏。两个线程各跑 5 轮,每轮打一行就 `yield`:

```text
   时间 →
   boot ──run_first──► A.it0 ──yield──► B.it0 ──yield──► A.it1 ──yield──► B.it1 ── ... ──► A.done ──(return→exit_current)──► B.it4 ──► B.done ──► (空)halt
                        │              │              │              │
                     切到 A          切到 B          切到 A          切到 B

   串口看到:
     [A] thread_a iteration 0
     [B] thread_b iteration 0
     [A] thread_a iteration 1
     ...
     [A] thread_a done
     [SCHED] Task tid=1 'thread_a' exited
     [B] thread_b done
     [SCHED] Task tid=2 'thread_b' exited
     [SCHED] No more tasks, halting.
```

注意最后那行 `halting`——它揭示了一个 019 的真实特性,后面调试现场会展开:这个调度器**没有 idle 任务**,活儿干完就直接 `cli;hlt` 把机器停住,而不是回到引导它的 `boot` 流程。
