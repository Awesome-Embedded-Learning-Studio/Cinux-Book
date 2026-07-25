---
title: 01 · 导引:点亮什么、为什么、设计图
---

# 导引:点亮什么、为什么、设计图

## 这一章我们要点亮什么

核心是**把「换人」的发起者从线程自己,挪到时钟中断**。具体说,020 交付五块:

- **时钟驱动抢占**:`Scheduler` 新增 `tick()` / `schedule()` / `is_initialized()`,外加 `DEFAULT_TIME_SLICE = 2`。`PIT::irq0_handler` 发完 EOI 之后调 `Scheduler::tick()`;`tick()` 数节拍,每到 2 就 `schedule()` 一次。而 `schedule()` 把当前任务标回 `Ready`、`pick_next` 取下一个、先更新 `TSS.RSP0`、再 `context_switch`。最关键的一笔:`yield()` 从此不再自己挑下一个,而是直接转调 `schedule()`——也就是说,020 之后「主动让出」和「被时钟打断」走的是**同一条**切换路径。
- **`context_switch.S` 的 `sti` 修复**:在「换栈之后、`jmp` 到新任务之前」插一条 `sti`。这是本章最硬的一个 bug 修复,根因在中断门的硬件语义。
- **`idle` 任务**:队列空了不再 `cli;hlt` 停机,而是落到一个只 `hlt` 的 `idle_task`;`PerCPU` 占位同时落地,为多核先挖个坑。
- **`TSS.RSP0` 更新**:`GDT::tss_set_rsp0(uint64_t)` 在每次切到非 idle 任务时更新——接口先接上,等将来有 ring3 才真正生效。
- **`sync.hpp` 的 `Spinlock` 原语**:`acquire` / `release` / RAII `guard` 三件套,基于 GCC `__atomic` 内建。

边界要摆正,免得读者误会:020 的 Task 全是**内核线程**,`Task::addr_space` 仍然没人填,没有用户态、没有 ring3、没有系统调用;`RoundRobin::pick_next` **不读 priority**(`Task::priority` 字段和 `idle_task` 的 255 一样,只是「为以后留」);`block` / `unblock` 虽然实现了 API,但**没有任何调用方**真拿它们做阻塞同步;`Spinlock` 只**定义**了、**还没人用**;`PerCPU` 是单核静态全局、不是 GS 相对寻址的真 per-CPU 区。这些都是「接口先到位、能力后到位」的诚实状态,不是已经发挥作用。

## 为什么现在需要它

019 的局限一句话:线程要是不主动 `yield`,CPU 就永远是它的。演示里我们得靠每个 worker 循环里手动调 `yield()` 才能看到两个线程交替;真把 `yield()` 注释掉,`thread_b` 永远没机会跑。这种「靠自觉」的调度,在跑一个死循环的坏线程面前直接失效。

把换人的发起者交给时钟,听着只是「换个触发源」,但它带来了一个 019 没有的麻烦:**切换点从干净的函数边界,挪到了「中断可以打断的任意指令处」**。019 的 `context_switch` 之所以只存 callee-saved(`r15/r14/r13/r12/rbp/rbx`)+ `rsp` + `rip` 这 8 个值、不碰 `RFLAGS`,有一个隐含前提:切换永远发生在函数调用边界上,而 System V AMD64 ABI 保证「调用前后 `RFLAGS` 不是被调方的义务」——`RFLAGS` 不在 callee-saved 之列(`rbx/rbp/r12-r15`),编译器要么已经替调用方把要用的 flag 存好了,要么根本不在乎。协作式正好踩在这条假设上,所以不存 `RFLAGS` 没事。

可一旦切换是被时钟中断逼出来的,调用 `context_switch` 的就不再是 `yield()` 这个普通函数,而是 IRQ0 的中断处理程序。CPU 一进中断门,硬件会先把 `RFLAGS`(连同 `CS`/`RIP`)压栈、并**清掉 IF**——而 `context_switch` 只换 callee-saved 和 `rsp`/`rip`,根本不恢复 `RFLAGS`。结果就是:从这个中断上下文切出去的新任务,**继承了 IF=0**,从此屏蔽了所有可屏蔽中断,再也不会被下一次时钟打断。这个坑在本章「调试现场·案例二」里会以具体症状出现,`sti` 那条修复就是为它准备的。换句话说,「把调度挂到时钟」逼着我们重新审视 `context_switch` 对中断状态的态度——这是 cooperative 迈向 preemptive 时一个经典且几乎必踩的陷阱。

顺带回答两个「为什么」。为什么需要 `idle` 任务?因为 019 队列空了就 `cli;hlt` 永久停机,生产 demo 跑完直接把机器卡死;有了 `idle`,所有真任务都退场后还有地方歇,而且它不进就绪队列、不会反过来抢真任务。为什么现在就先把 `Spinlock` 定义出来却不急着用?因为 020 把切换挂到了时钟上之后,调度器、就绪队列、PIT 这些共享数据**理论上**已经可能被「中断打断 + 新任务」的路径碰到——虽然单核 + 中断门语义下「真并发」还没发生,但 021 一旦要审查并发安全,手边就得有这么一把原语可用。先备着,不演示加锁路径。

## 设计图

先看抢占是怎么被触发起来的。这是 020 的主轴,也是 `sti` 那条修复落点最清楚的一张图:

```text
   IRQ0 到来(每 10ms @ 100Hz)
        │ CPU 进 ISR: 压 SS/RSP/RFLAGS/CS/RIP 到被中断任务的栈, 清 IF(中断门语义)
        ▼
   irq0_stub  (IF=0)  ──call──►  pit_irq0_handler(frame)
                                     │
                              tick_count_++
                              PIC::send_eoi(0)      ◄── EOI 先发, 保证下一个 IRQ 能到
                                     │
                              Scheduler::tick()
                                     │  current_slice_++; 到 DEFAULT_TIME_SLICE(=2) 就:
                                     ▼
                              Scheduler::schedule()
                                     │  ① prev=current_, Running 标回 Ready
                                     │  ② next=RoundRobin::pick_next()
                                     │  ③ 空/同则回落 idle 或原任务(直接 return, 不切)
                                     │  ④ 同步 current_ / g_per_cpu.current / current_slice_
                                     │     切到非 idle 则 GDT::tss_set_rsp0(next->kernel_stack_top)
                                     ▼
                              context_switch(&prev->ctx, &next->ctx)
                                     │  换栈后、jmp 前:  sti   ◄── 本章核心修复
                                     ▼
                  ┌──────────────────┴──────────────────┐
                  ▼                                       ▼
      next 是全新任务                          next 是被打断过的任务
      ctx.rip=入口, jmp 进线程函数             ctx.rip=.restore, ret 链回到 ISR stub
      sti 让它以 IF=1 起跑, 能收到下一次时钟    → IRETQ 还原被压栈的原始 RFLAGS(IF=1)
                                              (sti 对它是无害冗余)
```

两条「退路」都通向「中断重新打开」:全新任务靠那条 `sti`,被抢占过的任务靠 `IRETQ` 把压栈的旧 `RFLAGS`(IF=1)还回去。这正是 `sti` 只加一处、却能让所有任务都正常的关键。

再看时间片轮转的实际节奏,和 019 的严格交替形成对比:

```text
   019(协作式):线程自己 yield 才切, 严格交替
     A0 A1 A2 A3 A4 done │ B0 B1 ... B4 done │ halt

   020(抢占式):6 线程被时钟在忙循环中间打断, 谁先到 2 个 tick 谁让位
     A.it1 ~~~ [tick][tick] ▶ B.it1 ~~~ [tick][tick] ▶ C.it1 ~~~ ...
                (A 的忙循环没跑完就被切走; 稍后轮回来从 .restore 继续)
   串口看到的不再是 A 整段跑完才轮到 B, 而是 A/B/C/... 被 20ms 节拍交错打断
```

最后是 `TSS.RSP0` 在切换里的角色,得诚实标注它的现状:

```text
   切到新任务前:  GDT::tss_set_rsp0(next->kernel_stack_top)
                   └─► 直接写 g_gdt.tss_.rsp[0]

   TSS.RSP0 的语义(SDM §6.12.1): 特权级升高(ring3→ring0)时, 硬件从 TSS 取新栈顶
   ┌─────────────────────────────────────────────────────────┐
   │ 020 现状: 全程 ring0 内核线程, 不发生特权级变化            │
   │          → 这条更新现在其实不会触发硬件换栈                  │
   │          → 但接口先接上是「对的」, 等 ring3/用户进程来了就生效 │
   └─────────────────────────────────────────────────────────┘
   (切到 idle 时跳过 tss_set_rsp0: idle 没有独立内核栈要登记)
```
