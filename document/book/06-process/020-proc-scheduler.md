---
title: 020 · 时钟到点,该换人了:抢占式调度
---

# 020 · 时钟到点,该换人了:抢占式调度

> 上一章(019)我们造出了 `Task`、`context_switch`、`RoundRobin`,内核第一次有了多条活着的执行流——可痛也在这儿:它是**协作式**的,线程不主动 `yield()`,它就霸着 CPU 到天荒地老。真实的系统不能指望每个线程都自觉。这一章把调度器挂到那个从 011 章起就在跑的 PIT 时钟中断上,让 IRQ0 在固定节拍强行打断当前线程、强制换人——也就是**抢占式**多任务。顺带把 `idle` 任务、`TSS.RSP0` 更新、`PerCPU` 占位和最朴素的 `Spinlock` 原语一并铺上。中途会撞见一个从协作式升级到抢占式时几乎必踩的坑:从中断上下文里复用 `context_switch`,新任务会带着 `IF=0` 起跑、再也收不到下一次时钟——我们用一条 `sti` 把它修掉。做完,你会看到 6 个线程被时钟中断交错打断、不再严格排队跑完。

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

## 代码路线

### tick 与 schedule:让时钟来点名

[pit.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/pit/pit.cpp) 的 `irq0_handler` 末尾只多了一行(加一个 include),但这一行就是协作→抢占的总开关:

```cpp
void PIT::irq0_handler(InterruptFrame* /*frame*/) {
    tick_count_++;
    PIC::send_eoi(0);                 // 先 EOI: 让 PIC 准备好送下一个 IRQ
    cinux::proc::Scheduler::tick();   // 再调度: 在 tick 里可能切走当前任务
}
```

顺序为什么是「先 EOI 再 tick」?因为 `tick()` 一旦走到 `schedule()`,就可能 `context_switch` 切到另一个任务,很久不回来。如果先切再 EOI,PIC 还以为上一个 IRQ 没处理完,下一个时钟就送不进来——抢占直接哑火。先把 EOI 发了,再让调度器去折腾切人,这条时序不能反。

[scheduler.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/scheduler.cpp) 的 `tick()` 很短,职责就一件——数节拍、到点喊 `schedule()`:

```cpp
void Scheduler::tick() {
    if (!initialized_ || current_ == nullptr) return;   // 没就绪/没任务就不动
    tick_count_++;
    current_slice_++;
    if (current_slice_ >= DEFAULT_TIME_SLICE) {         // DEFAULT_TIME_SLICE = 2
        current_slice_ = 0;
        schedule();
    }
}
```

`current_ == nullptr` 那条守卫不是多余的:`main.cpp` 里必须**先** `Scheduler::init()` + 建好所有任务、**再** `PIC::unmask(0)` + `sti`。顺序颠倒,时钟中断会在 `current_` 还没就位时炸进来,`schedule` 里拿 `prev = current_` 就空指针了。生产代码里那行 `sti` 出现在「6 个任务都 `add_task` 之后」,正是这个顺序约束的体现。

真正干活的是 `schedule()`,它把「标回 Ready → 取下一个 → 同步状态 → 换栈」串成一条:

```cpp
void Scheduler::schedule() {
    if (current_ == nullptr) return;
    Task* prev = current_;

    if (prev->state == TaskState::Running)
        prev->state = TaskState::Ready;            // 让出 CPU, 回就绪队列里等着

    Task* next = default_rr_.pick_next();

    if (next == nullptr || next == prev) {          // 没别人 / 只剩自己
        if (prev->state != TaskState::Blocked && prev->state != TaskState::Dead) {
            prev->state = TaskState::Running;       // 自己接着跑, 不切
            return;
        }
        if (idle_task_ != nullptr && idle_task_ != prev) {
            next = idle_task_;                       // 真没活儿了, 落到 idle
        } else {
            return;
        }
    }

    current_ = next;
    g_per_cpu.current = next;                        // 同步 PerCPU 占位
    current_slice_ = 0;                              // 新任务重新计时

    if (next != idle_task_)
        cinux::arch::GDT::tss_set_rsp0(next->kernel_stack_top);

    context_switch(&prev->ctx, &next->ctx);          // 真切: 进去 prev, 出来在 next 栈上
}
```

几个点值得停一下。`next == prev` 那条分支是为了「只有自己一个任务」时不做无谓切换——`RoundRobin::pick_next` 会把唯一的任务轮到自己头上,这时与其假切一次,不如原地继续。落到 `idle` 的判断放在「`prev` 已经 `Blocked`/`Dead`」之后:当前任务只是普通让出、队里又有别人,不会走到 idle;只有真的没人可切、且自己又不能继续(阻塞或死亡),才把 CPU 交给 idle。`current_slice_ = 0` 看着琐碎,却是公平的关键——不归零,新任务一上来就可能因为 `prev` 残留的计数被立刻切走。

而 019 里那个自己挑下一个的 `yield()`,020 里退化成了 `schedule()` 的别名:

```cpp
void Scheduler::yield() {
    if (current_ == nullptr) return;
    schedule();   // 主动让出和被时钟打断, 走同一条路
}
```

这是一笔重要的简化:从此「谁下一个」的逻辑只有一份(`schedule`),不管触发源是 `yield` 还是 IRQ0。少一条路径,就少一种「两处逻辑不一致」的 bug。

### context_switch.S 的 sti:从中断上下文切出去,必须把中断打开

这段是本章的灵魂。[context_switch.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/context_switch.S) 在恢复完 callee-saved、换完栈之后,跳转之前,新加了一条 `sti`:

```asm
    movq 48(%rsi), %rsp     # to->rsp → %rsp: 换栈, 执行流从此切到新任务
    sti                      # 开中断 —— 本章核心修复
    jmp *56(%rsi)            # 跳到 to->rip(全新任务的入口, 或被打断任务的 .restore)
```

为什么协作式不写这条、020 非要写?根在「谁在调用 `context_switch`」。019 的调用者是 `yield()` ——一个普通函数,调用前后 IF 不变,`context_switch` 进来时 IF 该是多少还是多少,跳进新任务时继承的也是这个值,没问题。020 的调用者是 IRQ0 的中断处理程序——而 CPU 一进中断门,硬件会**清掉 IF**(SDM Vol.3A §6.12.1.3 原文:经中断门访问 handler 时,处理器清 IF 标志以防止其它中断干扰当前 handler;陷阱门则不清)。所以从中断上下文里调 `context_switch`,进来时 IF=0,换栈、`jmp` 进新任务后,新任务继承了 IF=0——它再也收不到下一次时钟,抢占在它身上永久失效。

`sti` 在这里干两件事。对**全新任务**(第一次被切到,`ctx.rip` 是线程入口):它以 IF=1 起跑,时钟能正常打断它。对**被抢占过的任务**(恢复运行,`ctx.rip` 是 `.restore`):它恢复后沿 `ret` 链一路退回 IRQ0 stub、由 `IRETQ` 把压栈的旧 `RFLAGS`(IF=1)还回来——这条 `sti` 对它是个无害的冗余,因为紧接着 `IRETQ` 会重写 IF。

`sti` 紧贴 `jmp`、中间不夹别的指令,不是随手排的。STI 有一条「延迟一拍」的硬件语义:执行 STI 之后,中断要等**下一条指令执行完**才被响应。这一点 SDM Vol.2B 在 STUI 条目里用对比写明了——它说 STUI 的效果「立即生效,这与 STI 相反,后者的效果会延迟一条指令」。把 `sti` 直接接在 `jmp` 前,意味着「换栈 + 跳转」这一瞬不会被中断从中间劈开,切换是原子的;`jmp` 一落地,中断窗口才重新打开。这就是这条 `sti` 既能修 bug、又不会在切换中途给自己添乱的原因。

诚实说一句:这套方案不是完美无瑕。被抢占的任务恢复后,从 `.restore` 一路 `ret` 退到 IRQ0 stub、再到 `IRETQ`,这段退栈路径上 IF 已经被 `sti` 打开了——理论上存在一个极短的窗口,期间可能被嵌套中断命中。笔记 `002` 自己算过:100Hz 时钟间隔 10ms,这段退栈是微秒级,命中概率可忽略,**但不是零**。更精细的做法是把 `RFLAGS` 纳入 `CpuContext`、用 `pushfq`/`popfq` 在切换点显式保存恢复中断状态——那是将来的事,020 没做。本章只交付「一条 `sti` 修掉 IF 丢失」这个最简洁的版本,并保留这层诚实。

### idle 任务:队列空了也有地方歇

`init()` 用 `TaskBuilder` 造一个 idle 任务,入口只做一件事——死循环 `hlt`:

```cpp
void Scheduler::idle_entry() {
    while (true) {
        __asm__ volatile("hlt");   // 没活儿就睡, 等下一个中断(时钟)唤醒
    }
}

// init() 里:
idle_task_ = TaskBuilder()
    .set_entry(idle_entry)
    .set_name("idle")
    .set_priority(255)             // 最低优先级(虽然 020 还没读它, 留个语义)
    .build();
if (idle_task_ != nullptr)
    idle_task_->state = TaskState::Ready;
```

两个细节。第一,idle **不进就绪队列**——注意 `init()` 里只 `build()` 了它,没有 `add_task(idle_task_)`。为什么?因为 `RoundRobin::pick_next` 是个公平轮转,如果 idle 在队里,它就会和真任务一起被轮流选中,反过来抢占真任务的 CPU 时间。idle 只在 `schedule` / `exit_current` 发现「队里没人」时被**显式地**当作兜底选中(`next = idle_task_`),而不是从队列里冒出来。第二,切到 idle 时**跳过 `tss_set_rsp0`**——前面 `schedule` 里那句 `if (next != idle_task_)` 守的就是这个:idle 没有要登记的「下次进内核态用的栈」,它的 kernel_stack 是 `TaskBuilder` 默认给的那份,从不被硬件换栈路径用到(当前全程 ring0,见下一节)。

有了 idle,019 那个「队列空了就 `cli;hlt` 永久停机」的粗暴收尾就被替换掉了:`exit_current` 里真没任务时落 idle 而不是停机,机器保持可响应(还能收键盘中断、还能被时钟唤醒),而不是死掉。

### TSS.RSP0 与 GDT::tss_set_rsp0

[gdt.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/gdt.cpp) 新增一个直写的静态方法:

```cpp
void GDT::tss_set_rsp0(uint64_t rsp0) {
    g_gdt.tss_.rsp[0] = rsp0;   // 直接写 TSS 里 ring0 的栈顶槽
}
```

`TSS` 结构体([gdt.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/gdt.hpp))按 SDM Vol.3A Figure 8-11 / §8.7「Task Management in 64-bit Mode」摆好 104 字节(源码注释里把它标成「Table 8-2」,但 SDM 实际以 Figure 8-11 呈现这张表),`rsp[3]` 是三个特权级的栈顶(ring0 用 `rsp[0]`)。每次 `run_first` / `schedule` / `exit_current` 切到非 idle 任务,都调一次 `tss_set_rsp0(next->kernel_stack_top)`。

为什么要这么干?SDM §6.12.1 说:当 handler 要在**更低特权级**(数值更大,即 ring3→ring0)执行时,处理器会从当前任务的 TSS 取 handler 要用的新栈顶(`SS:RSP`)。也就是说,RSP0 是「下一次从用户态掉进内核态时,硬件自动换上的那个内核栈顶」。既然每个任务有自己的内核栈,切到新任务时就得把 RSP0 指向新任务的内核栈顶,否则将来真有用户态进程时,缺页、系统调用掉进内核会用到**上一个任务**的内核栈,栈错位直接炸。

但必须如实说:020 全程是 ring0 内核线程,不发生任何特权级变化,所以这条 `tss_set_rsp0` **现在其实不会触发硬件换栈**——硬件压根没走到「从 TSS 取栈」那一步。它是个「接口先接上、等将来 ring3 来了再真正生效」的动作。写它、调它,是为了将来有用户进程时这块不用再回来补;不是因为它现在已经在保护什么。

### PerCPU 占位:为多核先挖个坑

[per_cpu.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/per_cpu.hpp) 整个文件就这么点东西:

```cpp
struct PerCPU {
    Task* current;          // 当前在跑的任务
    uint64_t kernel_stack;  // 内核栈顶(留给将来 RSP0 登记)
};

extern PerCPU g_per_cpu;    // scheduler.cpp 里定义: PerCPU g_per_cpu{nullptr, 0};
```

每次切换,`schedule` / `run_first` / `exit_current` 都同步一句 `g_per_cpu.current = next;`。得诚实讲清楚它**不是**什么:它不是 GS 基址相对寻址的真 per-CPU 区,也不是每 CPU 独立运行队列,就是一个**单核静态全局变量**。020 只有一个 CPU,放它纯粹是为了让「将来 `current` 从全局迁移到 per-CPU」时改动小——先把读取入口统一到 `g_per_cpu.current`,将来换成 GS 相对寻址时,只动这一个定义,调用点不用大改。别把它说成 SMP 地基,它现在连第二份实例都没有。

### sync.hpp:Spinlock 原语,先定义着

[sync.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/sync.hpp) 的 `Spinlock` 三件套:

```cpp
class Spinlock {
public:
    void acquire() {
        while (__atomic_test_and_set(&locked_, __ATOMIC_ACQUIRE))  // 原子置 1 并返回旧值
            __asm__ volatile("pause");                              // 自旋提示, 降功耗、避免乱序违例
    }
    void release() {
        __atomic_clear(&locked_, __ATOMIC_RELEASE);                // 原子清 0
    }
    [[nodiscard]] auto guard() { return Guard(this); }             // RAII: 构造 acquire, 析构 release
private:
    volatile bool locked_ = false;
    class Guard { /* 构造 acquire / 析构 release / 禁拷贝禁赋值 */ };
};
```

`__atomic_test_and_set` 是「把目标字节原子地置 1、并返回它的旧值」的标准内建,在 x86 上编译成带 `LOCK` 前缀的 `xchg` 或等价指令;`__ATOMIC_ACQUIRE` / `__ATOMIC_RELEASE` 配对保证「拿到锁之后读到的内存视图」和「释放锁之前的写」按正确的可见序传递。`pause` 是给超线程 CPU 的提示:告诉硬件「我在自旋,别把整个流水线占满」,顺便避免一段长自旋触发内存序违例惩罚。`[[nodiscard]] auto guard()` 让调用方写成 `auto g = lock.guard();`,出了作用域自动释放,忘不了。

定性很重要:020 只**定义**了 `Spinlock`,**没有任何代码用它**。调度器、就绪队列、PIT 计数器——全都还是裸的、没加锁。它是为 021「立刻审查现有组件的并发安全性」备的原语,本章不演示一段加了锁的调度路径。看到这个类存在,不等于它已经在保护什么。

## 调试现场

两条真实笔记,都压成「症状→根因→定位→修复→防复发」。它们恰好是抢占式上线时最常遇到的两种「看着能跑、其实没生效」。

### 案例一:时间片过长,抢占从未触发

症状是 3 个线程各跑 5 轮,**完全顺序**执行,串口上 A 整段跑完才轮到 B,B 跑完才轮到 C,没有任何交错——和 019 的协作式 demo 看起来一模一样,仿佛时钟根本没接上调度器。

根因不在调度器,而在「时间片和负载的配比」。PIT 配 100Hz(每 tick 10ms),当时 `DEFAULT_TIME_SLICE = 10`,也就是 **100ms 才触发一次抢占**。而忙循环 `for (volatile int j = 0; j < 1000000; j++) {}` 在 QEMU TCG 模式下极快,单次迭代不到 5ms,5 轮加起来 < 50ms——线程在自己的 100ms 时间片**之内**就跑完了,时钟压根没机会在它跑的过程中打断它。所以现象是「顺序跑完」,但原因不是「没抢占」,而是「负载太轻、时间片太长,抢不到点上」。

修复两处一起改:`DEFAULT_TIME_SLICE` 从 10 调到 2(20ms 时间片),忙循环从 100 万次提到 2000 万次(让每个线程的工作量明显跨过多个时间片)。两者都改,是为了从两头把「线程在片内跑完」的可能挤掉。

防复发的教训:在虚拟化环境里,简单的 CPU 密集循环比裸机预期快得多。测抢占时,要么把**负载做大**、要么把**时间片做小**,让定时器有机会在任务执行中途介入——否则你看到的「顺序执行」会骗你以为抢占没生效,而去查调度器,其实调度器一直好好的。

### 案例二:context_switch 丢了 IF,后续线程中断全关

这个比案例一阴险得多。症状是 6 个线程 × 10 轮 × 2000 万忙循环:第一次抢占成功了(A 跑了 2 次后被切到 B),但**之后 B/C/D/E/F 全部顺序跑完、再也没被抢占过**——只有 A 这一个被抢占过的任务能恢复中断,其余新启动的线程都带着 IF=0 一路跑到底。

根因要接上前面代码路线讲过的硬件语义。`context_switch.S` 只存 callee-saved + `rsp`/`rip`,**不碰 RFLAGS**——这在协作式下没问题(切换在函数边界,IF 不变)。但抢占的调用链是:

```text
IRQ0 → ISR stub(CPU 进中断门, 清 IF) → pit_irq0_handler → Scheduler::tick → schedule → context_switch
```

CPU 一进中断门就清 IF(SDM §6.12.1.3),所以 `context_switch` 是在 IF=0 的状态下被调用的。它换栈、`jmp` 进新任务时,把 IF=0 一起带过去了。于是两种任务的命运分叉:

- **全新任务**(`ctx.rip` = 入口,第一次被切到):`jmp` 直接跳进线程函数,IF 仍为 0,再也收不到时钟中断 → 永不被抢占。这正是 B–F 的遭遇。
- **被抢占过的任务**(`ctx.rip` = `.restore`):恢复后 `ret` 一路退回 IRQ0 stub,由 `IRETQ` 把压栈的旧 `RFLAGS`(IF=1)还原(SDM §6.12.1:IRET 把保存的标志恢复进 EFLAGS)→ 中断恢复。这正是 A 能正常的原因。

所以只有第一个被抢占的 A 靠 `IRETQ` 救了回来,后面所有新启动的线程全带着 IF=0。定位的关键就是认出「只有被抢占过的任务正常、全新任务都哑」这个非对称——它直接指向「切换瞬间中断状态没保证」。

修复就是在 `context_switch.S` 换栈之后、`jmp` 之前加一条 `sti`,让新任务以 IF=1 起跑。为什么对被抢占过的任务也安全?因为它们随后会走 `IRETQ` 重写 IF,`sti` 是冗余;为什么不会引入新麻烦?因为 `sti` 紧贴 `jmp`,STI 的「延迟一拍」语义保证换栈+跳转这一瞬不被中断劈开(见代码路线那节)。

防复发的教训:协作式的 `context_switch` 设计时**不碰 RFLAGS 是合理的**——它永远在明确的调用点切,IF 不变。可一旦**从中断上下文里复用同一个 `context_switch`**,就必须保证新任务的中断状态正确。这是 cooperative 迈向 preemptive 的经典陷阱,凡是「把现成的协作式切换直接塞进中断 handler」的实现,几乎都要在这一条上栽一次。更彻底的修法是把 `RFLAGS` 纳入 `CpuContext`、用 `pushfq`/`popfq` 显式保存恢复——020 没做,留作将来。

## 验证

先说 host 侧的覆盖现状,得如实:**020 没有新增独立的 host 单元测试**。019 那组镜像 `test/unit/test_scheduler.cpp`(测 `RoundRobin`/`Scheduler`/`TaskBuilder`/`CpuContext` 的纯逻辑)在 020 的 diff 里未改动,host 侧覆盖仍停留在那一组。020 新增的 `tick`/`schedule`/`block`/`unblock` 这些**没有 host 镜像**——它们要么依赖 `context_switch` 的真汇编换栈,要么依赖中断/PIT 的真硬件语义,只能在 QEMU 里验。

```bash
# host 侧(019 那组, 020 未变)
ctest --test-dir build -R scheduler --output-on-failure
```

真正的验证在 QEMU 机内。[test_scheduler.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_scheduler.cpp) 跑在真 PMM/VMM/Heap 之上,节名从 019 的 `(019)` 改成了 `(020)`,并在原有用例之后新增 Test 9 `test_scheduler_new` 三个用例:

```bash
cmake --build build --target run-big-kernel-test
```

机内 `TEST_SECTION("Scheduler/Process Tests (020)")` 应全过,其中新增三项分别盯:`test_is_initialized`(`init()` 后 `is_initialized()==true`)、`test_remove_task`(`add` 后 `remove`,state 变 `Dead`)、`test_block_unblock`(`add` 后 `block`→`Blocked`、`unblock`→`Ready`)。注意这三个是**状态机/接口**层面的验证,不触发真抢占——抢占只能靠下面的生产 demo 用肉眼验。

最后是**生产 demo**,这是抢占是否真生效的唯一肉眼验证。跑大内核:

```bash
cmake --build build --target run
```

串口应该看到 `[A]` / `[B]` / `[C]` … 六个线程被时钟中断**交错**打断——不再是 019 那种 A 整段跑完才轮到 B 的严格顺序,而是某个线程的忙循环跑到一半就被切走、换另一个线程的输出插进来、稍后再轮回来。每个线程最后各自打一句 `done`。如果你看到的不是交错、而是「只有第一个线程被抢占、其余顺序跑完」,那就是撞上了**案例二那条 IF 丢失的 bug**——`sti` 没接上,回去查 `context_switch.S`。

## 下一站

020 把切换挂到了时钟上,「谁让出 CPU」不再靠线程自觉。可代价也摆在眼前:调度器、就绪队列、PIT 计数器这些共享数据,**理论上**已经暴露在「被中断打断、又被新任务碰」的并发路径下了——虽然单核 + 中断门语义下「真并发」还没发生,但只要再多一个执行源(多核、或者中断里真去动队列),竞态就会冒头。而我们现在连一把锁都没真正用上。

下一站(021)就治这个:基于 020 备好的 `Spinlock`,把 `Mutex`、`Semaphore`、等待队列落地,让 `block`/`unblock` 真正派上用场(线程因为等 I/O、等锁而阻塞,被唤醒源重新 enqueue),并立刻对现有组件做一遍并发安全审查。`PerCPU` 也还要从「单核全局」长成「真 per-CPU」;至于更高半区的 ring3、系统调用、独立地址空间切换,再往后。020 的时钟 + `sti` + `idle` 是那一切的节奏地基——节奏先稳,上面才好盖并发。

---

### 参考

- **Intel SDM Vol.3A §6.12.1 "Exception- or Interrupt-Handler Procedures"**(本地 `document/reference/intel/SDM-Vol3A-System-Programming-Guide-Part1.pdf`,PDF 第 209 页 / 书内 6-13 页,已读到正文):进入 handler 时处理器把 `EFLAGS`/`CS`/`EIP` 压栈,特权级变化时 handler 栈「从当前任务的 TSS 获得」;`IRET`「把保存的标志恢复进 EFLAGS」。支撑「中断门进入时 IF 被清、`IRETQ` 还原 IF」与「`tss_set_rsp0` 的硬件依据」两条。
- **Intel SDM Vol.3A §6.12.1.3 "Flag Usage By Exception- or Interrupt-Handler Procedure"**(同 PDF,第 213 页 / 书内 6-17 页,已读到原文):「经中断门访问 handler 时,处理器清 IF 标志以防止其它中断干扰当前 handler……后续 IRET 把 IF 恢复为栈上保存值;陷阱门不影响 IF」。支撑「IRQ0 stub 一进去就是 IF=0」这条根因,以及案例二的非对称现象。
- **Intel SDM Vol.2B STI 的「延迟一拍」语义**(本地 `document/reference/intel/SDM-Vol2B-Instruction-Reference-M-U.pdf`,STUI 条目 PDF 第 691 页 / 书内 4-683 页,已读到正文):STUI 条目以对比方式写明「STI 的效果延迟一条指令」。支撑「`sti` 紧贴 `jmp`,换栈+跳转这一瞬不被中断劈开」的设计正确性。
- **GCC `__atomic` Builtins**(GCC 在线手册 `https://gcc.gnu.org/onlinedocs/gcc/_005f_005fatomic-Builtins.html`):`__atomic_test_and_set`(原子置 1 并返回旧值)、`__atomic_clear`(原子清 0)、`__ATOMIC_ACQUIRE`/`__ATOMIC_RELEASE` 内存序。支撑 `sync.hpp` 的 `Spinlock` 实现。
- **OSDev Wiki "Context Switching" / "Spinlock"**(`https://wiki.osdev.org/Context_Switching`、`https://wiki.osdev.org/Spinlock`,域名 200 在线):从中断 handler 里触发 schedule 的通用思路、`test_and_set` + `pause` 的自旋锁写法,概念性对照。
- **xv6-riscv**(仓库 `https://github.com/mit-pdos/xv6-riscv`):时钟中断在 trap 处理里触发 `yield` 的对照——切换点从函数边界挪到中断返回路径,与本章设计同源。
- **System V AMD64 ABI**(`https://gitlab.com/x86-psABIs/x86-64-ABI`):callee-saved(`rbx/rbp/r12-r15`)约定——`CpuContext` 只存这 6 个 + `rsp`/`rip`,而 `RFLAGS` 不在 callee-saved 之列,这正是「协作式 `context_switch` 本不碰 RFLAGS、抢占式才要补 `sti`」的根。延续 019 章已核引用。
- **019 章 · [让内核长出第二条执行流:进程上下文](019-proc-context.md)**:`Task`/`context_switch`/`RoundRobin`/higher-half 地基,本章直接接续;`CpuContext` 布局与 callee-saved 论证亦出自此。
- 本 tag 源码:[scheduler.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/scheduler.hpp) / [scheduler.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/scheduler.cpp)、[context_switch.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/context_switch.S)、[gdt.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/gdt.hpp) / [gdt.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/gdt.cpp)、[pit.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/pit/pit.cpp)、[per_cpu.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/per_cpu.hpp)、[sync.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/sync.hpp)、[main.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/main.cpp);测试 [test_scheduler.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_scheduler.cpp)(QEMU 机内,节名 `(020)`)。
