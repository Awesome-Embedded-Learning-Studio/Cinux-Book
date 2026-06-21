---
title: Lab 020 · 时钟到点,该换人了:抢占式调度
---

# Lab 020 · 时钟到点,该换人了:抢占式调度

> 配套章节:[020 · 时钟到点,该换人了:抢占式调度](../../book/06-process/020-proc-scheduler.md)。这一关给你目标和约束,不贴 `context_switch.S` 的 `sti` 那段汇编、不贴 `schedule()` 五步切换的完整实现、不贴 `Spinlock` 的成品类——那些得你自己想明白为什么这么排、自己写出来、自己踩坑修出来。

## 实验目标

把 019 那台「线程不 `yield` 就独占 CPU」的协作式机器,升级成「时钟到点,调度器强行换人」的抢占式机器。核心是五块,缺一块都不算数:

1. **把调度器挂到 PIT 时钟中断上**。IRQ0 每到一个节拍就喂给 `Scheduler::tick()`,攒够 `DEFAULT_TIME_SLICE` 就 `schedule()` 一次——不再等线程自觉。
2. **统一「主动让出」与「被时钟打断」**。`yield()` 退化成 `schedule()` 的别名,让两条路径走的是同一个切换流程,别给未来的 bug 留两条岔路。
3. **让 idle 任务兜底空队列**。所有真任务都跑完了,别像 019 那样 `cli; hlt` 直接停机——得有个永远 Ready、priority 最高(255)、只 `hlt` 的 idle 垫底。
4. **接上 TSS.RSP0 的更新接口**。每次切到非 idle 任务,把 `tss_.rsp[0]` 指向新任务的内核栈顶。
5. **修掉从中断上下文切换会丢 IF 的那个坑**。换栈之后、`jmp` 到新任务之前,补一条 `sti`。这是这一关最硬的一锤,不做就把抢占做成了「只有第一个线程能被抢占」的笑话。

做完这五块,内核就有了「任意指令处被时钟打断、切到别人、回头还能接着跑」的能力。但要把期望放正:这一关**只**做了抢占的骨架。`Spinlock` 你会定义出来,可全内核没有一个地方真正用它(那是 021 的活);`PerCPU` 是单核静态全局占位,不是真 per-CPU 区域;`tss_set_rsp0` 这个接口接上了,但因为现在全程 ring0、根本不发生特权级变化,硬件其实还不会真正靠它换栈——它是个「先把口子留好,等 ring3 来了就生效」的提前动作。诚实认清这几处「定义了但不生效」,比假装它已经发挥作用重要得多。

## 前置条件

你得先过 Lab 019。019 给你留下的地基是这一关全部的依赖:

- **019 的 `Task` / `CpuContext` / `TaskBuilder`**:这一关不碰它们的结构,直接复用。
- **019 的 `context_switch.S`**:这一关要在它身上动刀(加 `sti`),但前提是它本身已经能干净切换 callee-saved + 换栈 + `jmp`。
- **019 的 `RoundRobin` / `Scheduler` 门面**:`init` / `add_task` / `yield` / `exit_current` / `run_first` 都要在这一关扩成抢占版。
- **019 的 higher-half 收口**:大内核已经在它链接的 higher-half 地址跑,这一关的 demo、TSS 更新都站在这个地基上。

还要确认两件更早的事没掉链子:**011 的 PIT**(`PIT::init(100)` 把 IRQ0 配成 100 Hz,`pit_irq0_handler` 每个 tick 都进)、**007 的 IDT/GDT**(IRQ0 的中断门已经注册、TSS 已经 `ltr` 加载)。019 的 `main` 里 `sti` 之后,IRQ0 其实一直在触发、一直在进 `pit_irq0_handler`,只是那时候 handler 只递增 tick 计数、没碰调度器——这一关要做的,就是把那一行 `Scheduler::tick()` 接上。

外部约定上,这一关和硬件契约最紧的一条是 **Intel SDM Vol.3A §6.12.1**:CPU 进 ISR 时把 `RFLAGS/CS/RIP` 压栈并清 IF(interrupt-gate 语义),`IRETQ` 才还原。这条是 `sti` 修复的全部依据,动手前最好心里有数。

## 任务分解

**第一步:在 `pit.cpp` 的 `irq0_handler` 末尾接上调度器。** `PIT::irq0_handler` 原本只 `tick_count_++` 然后 `PIC::send_eoi(0)`。这一关在 `send_eoi(0)` **之后**追加一行 `Scheduler::tick()`,并 include scheduler 头。这里的顺序是一个铁约束:**先 EOI,再 tick**。EOI 告诉 PIC「这个 IRQ 我收下了、你可以放下一个」,先发 EOI 才保证下一个节拍的 IRQ0 还能到;如果把 `tick()` 放在 EOI 之前,而 `tick()` 里触发的 `schedule()` 又 `context_switch` 切走了,那 EOI 可能要等很久才发,下一个时钟中断就卡住了。

**第二步:`scheduler.hpp` 上的新成员。** 给 `Scheduler` 加这些:一个 `static constexpr int DEFAULT_TIME_SLICE = 2`(为什么是 2 而不是更大的数,见「常见故障」案例一)、`tick()` / `schedule()` / `is_initialized()` 三个公开方法、`block(Task*, const char*)` / `unblock(Task*)` / `remove_task(Task*)` 三个状态机方法,以及私有成员 `idle_entry()`、`idle_task_`、`initialized_`、`tick_count_`、`current_slice_`。`yield()` 改成转调 `schedule()`——把「主动让出」和「被时钟打断」并到一条路径。

**第三步:`init()` 造 idle 任务。** 在 `init()` 注册完默认 `RoundRobin` 之后,用 `TaskBuilder` 造一个 `idle_task`:入口 `idle_entry`(一个死循环 `hlt`)、`set_priority(255)`、`build` 出来后把 state 设回 `Ready`。**关键:这个 idle 任务不要 `add_task` 进就绪队列**。想清楚为什么——`RoundRobin::pick_next` 会出队头又塞回队尾,如果 idle 在队列里,它早晚会轮到、把真任务挤掉。idle 是「兜底」,不是「候选」;它只能被 `schedule`/`exit_current` 在 `pick_next` 返回空时**显式落到**,绝不能自己排队。最后把 `initialized_ = true`。`is_initialized()` 就返回这个旗标。

**第四步:`tick()` 和 `schedule()`——把切换挂到节拍上。** `tick()` 的逻辑很轻:先守一个卫兵(`!initialized_ || current_ == nullptr` 就直接返回,防止时钟在 `current_` 还没就位时硬切),然后 `tick_count_++` / `current_slice_++`,一旦 `current_slice_ >= DEFAULT_TIME_SLICE` 就清零并 `schedule()`。`schedule()` 是这一关切换的真正落地处,五步走完,顺序不能乱:(1) `prev = current_`,把 prev 的 `Running` 标回 `Ready`;(2) `pick_next()` 取下一个;(3) 如果 `next` 为空或就是 `prev` 自己——要么 prev 还能跑就让它继续(状态恢复 Running 后直接 return,不切),要么落 idle;(4) 切之前同步 `current_` / `g_per_cpu.current` / `current_slice_=0`,并在「切到非 idle 任务」时调 `GDT::tss_set_rsp0(next->kernel_stack_top)`;(5) `context_switch(&prev->ctx, &next->ctx)`。整段完整实现这一关不贴,但「先存 prev、再判断、最后切」这个骨架你得自己补齐。

**第五步:`context_switch.S` 加 `sti`——这一关最硬的一锤。** 在你已经写好的协作式 `context_switch` 基础上,定位到「换栈之后、`jmp *56(%rsi)` 之前」那个位置,插一条 `sti`。也就是这三行的相对顺序是死的:`movq 48(%rsi), %rsp`(换到新任务栈)→ `sti`(开中断)→ `jmp *56(%rsi)`(跳到新任务 `rip`)。**为什么协作式不需要、抢占式非写不可**,得用硬件语义想通:019 的协作式切换总发生在函数边界,调用 `context_switch` 时 IF 不变(开着还是开着);可一旦同一个 `context_switch` 被从 IRQ0 的中断上下文调用,情况就变了——CPU 进 ISR 时已经清了 IF(interrupt-gate),`context_switch` 又不碰 `RFLAGS`,于是新任务继承了 IF=0、再也收不到下一次时钟中断。`sti` 就是专门救「全新启动的任务」的:让它以中断开启起跑。而那些**被抢占过、又回来恢复**的任务呢?它们走 `.restore` 标号 → 沿 `ret` 链退回 ISR stub → 最终 `IRETQ` 从中断帧还原原始 RFLAGS(IF=1)。对它们来说,`sti` 是无害的冗余。

**第六步:`gdt.cpp` 的 `tss_set_rsp0`。** 加一个 `static void GDT::tss_set_rsp0(uint64_t rsp0)`,函数体就一行:把参数写进 `g_gdt.tss_.rsp[0]`。`run_first`、`schedule`、`exit_current` 三处,在切到**非 idle**任务前都调它。这里要诚实:RSP0 是「特权级升高时硬件自动换的栈顶」,SDM 说栈从 TSS 取——换了任务当然得换 RSP0。可这一关全程 ring0,不发生特权级变化,所以这条更新**现在并不会真正触发硬件换栈**,它只是把接口接对。等将来有了 ring3 用户进程、有了栈切换,这条才真正生效。

**第七步:`per_cpu.hpp` 占位 + `sync.hpp` 的 `Spinlock` 原语。** 新建 `per_cpu.hpp`,`struct PerCPU { Task* current; uint64_t kernel_stack; }` 加一个 `extern PerCPU g_per_cpu;`,在 `scheduler.cpp` 里定义 `PerCPU g_per_cpu{nullptr, 0}`。每次切换同步 `g_per_cpu.current = next`。别把它当成 SMP 地基——它是单核静态全局占位,放它的意义只是「将来 `current` 从全局迁到真 per-CPU 区域时改动小」。`sync.hpp` 里定义 `class Spinlock`:`acquire()` 用 `__atomic_test_and_set(&locked_, __ATOMIC_ACQUIRE)` 自旋、失败时 `pause` 提示;`release()` 用 `__atomic_clear(&locked_, __ATOMIC_RELEASE)`;`guard()` 返回一个 RAII `Guard`(构造 acquire、析构 release、禁拷禁赋值)。**这一关只定义它,不许在调度路径里调用它**——全内核没有任何代码用它保护数据结构,那是 021 并发审查的活。

**第八步:`main.cpp` 的抢占 demo。** 把 019 的「两线程各打 5 轮手动 `yield`」换成「6 个线程(`thread_a`..`thread_f`)× 10 轮迭代 × 2000 万次忙循环,纯靠时钟中断交替」。worker 里用 `Scheduler::current()->tid` 自报家门。**顺序是铁律:先 `Scheduler::init()`、先 `build` 出 6 个任务并 `add_task`,再 `PIC::unmask(0)` / `PIC::unmask(1)`、最后 `sti`**。顺序反了会出事——时钟中断抢在一个 `current_` 还没就位的状态触发,`tick()` 的卫兵虽然挡得住,但这是侥幸,正确姿势就是先把世界搭好再开中断。

## 接口约束

你要实现/改动出来的东西,对外长这样(职责与签名,不给实现):

- `PIT::irq0_handler(InterruptFrame*)`:`tick_count_++` → `PIC::send_eoi(0)` → `Scheduler::tick()`(三步顺序死)。
- `Scheduler` 新增公开:`static void tick(); static void schedule(); static bool is_initialized(); static void block(Task*, const char* reason); static void unblock(Task*); static void remove_task(Task*);` 新增常量 `static constexpr int DEFAULT_TIME_SLICE = 2;`
- `Scheduler::yield()`:转调 `schedule()`(020 之后 yield 不再自己挑下一个)。
- `Scheduler::schedule()`:五步切换(标回 Ready → pick_next → 空则落 idle → 同步 current_/g_per_cpu.current/current_slice_/TSS.RSP0 → context_switch)。
- `static void GDT::tss_set_rsp0(uint64_t rsp0)`:写 `g_gdt.tss_.rsp[0]`。
- `struct PerCPU { Task* current; uint64_t kernel_stack; }; extern PerCPU g_per_cpu;`(单核静态全局占位)。
- `class Spinlock`:`void acquire(); void release(); [[nodiscard]] auto guard();`(只定义,无调用方)。

关键约束(违反就翻车):

- **EOI 先于 tick**。`irq0_handler` 里 `send_eoi(0)` 必须在 `Scheduler::tick()` 之前,否则下一个 IRQ 卡住。
- **`sti` 落在「换栈之后、`jmp` 之前」之间**。放早了(换栈之前)无效,放晚了(`jmp` 之后到不了)也无效;它必须是换栈指令与跳转指令之间唯一的一条。
- **idle 任务不入就绪队列**。`init()` 里 `build` 出 `idle_task` 后只把 state 设回 `Ready`,**不调 `add_task`**。它在 `schedule`/`exit_current` 里被 `pick_next` 返回空时显式落到。
- **切到 idle 时跳过 `tss_set_rsp0`**。idle 是内核里的常驻死循环,没有独立的「下次进内核态要用的栈」概念;只有切到非 idle 任务才更新 RSP0。
- **先建任务再 `sti`**。`Scheduler::init()` + 6 个 `TaskBuilder().build()` + `add_task` 全部完成之后,才 `PIC::unmask` + `sti`。
- **`block`/`unblock`/`remove_task` 是状态机钩子,不是阻塞同步**。这一关它们只做 `state` 迁移 + enqueue/dequeue(+`block` 当前任务时顺带 `schedule`),**没有等待队列、没有唤醒源**。别在 demo 里演示「线程因 I/O 阻塞被唤醒」——那是 021 的事。
- **`Spinlock` 只定义、不调用**。整关任何代码路径里都不许出现 `lock.acquire()` / `lock.guard()` 的真实调用;它是给 021 备的原语。

具体汇编偏移(`movq 48(%rsi), %rsp` / `jmp *56(%rsi)`)、`schedule()` 五步里每一步的 `if` 怎么排、idle 的 priority 取 255 还是用别的魔数、忙循环用 2000 万还是别的量级——这些这一关不替你定死,但你定下来就要和 `CpuContext` 的 `static_assert`、和 `DEFAULT_TIME_SLICE`、和「抢占要真能触发」这件事对齐。

## 验证步骤

先说清 host 侧这一关的真相:**020 没有新增任何独立的 host 单元测试**。019 那一组 host 镜像(队列轮转 / `TaskBuilder` 守卫 / `CpuContext` 布局)在 020 的 diff 里没动,覆盖仍停留在 019。`tick` / `schedule` / `block` / `unblock` / `remove_task` 这些新逻辑**没有 host 镜像**,只在 QEMU 机内验。所以 host 侧这一关能跑的还是:

```bash
ctest --test-dir build -R scheduler --output-on-failure
```

但别指望它覆盖抢占——它验的是 019 那一批纯逻辑。

真正能验抢占的,是 QEMU 机内测。机内节名这一关从 `(019)` 变成 `(020)`,并在 Test 9 `test_scheduler_new` 里加了三个用例:`test_is_initialized`(init 后 `is_initialized()==true`)、`test_remove_task`(add 后 remove,state 变 `Dead`)、`test_block_unblock`(add 后 block→`Blocked`、unblock→`Ready`):

```bash
cmake --build build --target run-big-kernel-test
```

机内会打 `Scheduler/Process Tests (020)`,15 个用例全过、末尾 `ALL TESTS PASSED`。注意这三个新用例验的是状态机和初始化旗标,**不是抢占本身**——`test_remove_task` 只断言 add 后 `sched_class` 非空、remove 后 `task->state == TaskState::Dead`,它**并没有**再调 `pick_next` 去验证那个任务真的取不到了;`remove_task` 把任务标 `Dead` 时顺带从调度类 dequeue 的副作用,本关测试里并不直接覆盖,它依赖的是 `RoundRobin::dequeue` 自己的正确性(在 `test_round_robin::test_dequeue_middle` 里另外覆盖)。`block`/`unblock` 同样只验状态迁移。

抢占是否真的生效,只有一个肉眼验证:跑**生产 demo** 本身:

```bash
cmake --build build --target run
```

串口应看到 6 个线程(`[A]`..`[F]`)被时钟中断**交错**打断——某个线程没跑完就被切走、轮到下一个、回头再继续,而不是像 019 那样严格一个跑完再跑下一个。所有线程各自打完 `done`、最后进 idle。**只描述现象**——具体某一行串口输出什么,取决于你的忙循环量级和 `DEFAULT_TIME_SLICE`,别拿没验过的逐行日志当预期。一个明确的失败信号:如果串口显示「只有第一个被抢占的线程能恢复、其余线程顺序跑完再不被打断」,你撞上的是 IF 丢失那条 bug(`sti` 没加或加错位置),见下一节案例二。

## 常见故障

- **6 个线程完全顺序跑完,从头到尾没交错**:`DEFAULT_TIME_SLICE` 太大或忙循环太短,线程在自己一个时间片内就跑完了,定时器还没机会介入。根因是虚拟化下 CPU 密集循环比预期快得多(100 Hz × `DEFAULT_TIME_SLICE=10` = 100 ms 时间片,而 QEMU TCG 下千万级忙循环单轮往往不到 5 ms)。修复:把 `DEFAULT_TIME_SLICE` 调小(这一关定的是 2),或把忙循环量级加大(这一关 demo 用 2000 万)。防复发:测抢占要么负载够大、要么时间片够小,让定时器有缝插针。

- **只有第一个被抢占的线程能恢复中断,其余全程 IF=0 顺序跑完再不被抢**:你漏了 `sti`,或加错了位置。根因是 `context_switch` 只存 callee-saved、不碰 `RFLAGS`;被从中断上下文(IRQ0 stub 进来时已清 IF)切到的新任务继承 IF=0,永远收不到下一次时钟;只有被抢占过的任务能靠回到 `.restore` → `ret` 链 → `IRETQ` 还原原始 RFLAGS。修复:换栈之后、`jmp` 之前补一条 `sti`。防复发:协作式 `context_switch` 一旦被从中断上下文复用,就必须保证新任务的中断状态正确——这是 cooperative 迈向 preemptive 的经典陷阱。

- **`schedule` 里把 EOI 放到了 `tick` 之后,下一个时钟中断再也不来**:`send_eoi(0)` 必须先发。根因是 PIC 在收到 EOI 之前不会再投递同一线的 IRQ,而 `tick()` 触发的 `schedule()` 会 `context_switch` 切走,EOI 被无限期推迟,IRQ0 永久卡死。修复:`irq0_handler` 里 `send_eoi(0)` 紧跟在 tick 计数之后、`Scheduler::tick()` 之前。

- **idle 被误塞进就绪队列,真任务被它抢占、控制流卡在 idle 的 `hlt`**:`init()` 里对 `idle_task` 多调了一句 `add_task`。根因是 `pick_next` 出队头又塞回队尾,idle 早晚会轮到、把真任务挤掉。修复:idle 只 `build` 出来、把 state 设回 `Ready`,**不入队**;它只在 `schedule`/`exit_current` 里 `pick_next` 返回空时显式落到。

- **先 `sti` 再建任务,内核在 `current_` 还没就位时被时钟打断,行为不可预期**:`main` 里顺序写反了。根因是 `sti` 之后 IRQ0 立刻可能触发,而此时 `Scheduler::init()` 还没跑、`current_` 还是空指针。修复:`init()` + 6 个 `build` + `add_task` 全部完成后,才 `PIC::unmask` + `sti`。`tick()` 里那个「`current_ == nullptr` 就返回」的卫兵是第二道保险,不是你可以乱排顺序的理由。

- **把 `Spinlock` 当成「已经用于保护调度路径」来写**:越界。这一关 `Spinlock` 只定义、无调用方;调度器、PIT、就绪队列全都还没加锁。根因(认知上)是 020 只铺原语,真正上锁是 021 的并发审查。修复:删掉你在调度路径里写的任何 `lock.acquire()` / `lock.guard()` 调用,留 `Spinlock` 的定义在那里即可。

- **以为 `tss_set_rsp0` 现在就在硬件换栈了**:认知错。当前全程 ring0,不发生特权级变化,硬件不会靠 RSP0 换栈。它只是「接口接对、等 ring3 生效」的提前动作。修复:正文和注释里如实说「当前未真正触发硬件换栈」。

## 通过标准

1. `pit.cpp` 的 `irq0_handler` 在 `send_eoi(0)` 之后调 `Scheduler::tick()`;EOI 严格先于 tick。
2. `Scheduler` 有 `tick()` / `schedule()` / `is_initialized()`,常量 `DEFAULT_TIME_SLICE = 2`;`yield()` 转调 `schedule()`——主动让出与被时钟打断走同一条路径。
3. `schedule()` 五步切换到位:标回 Ready → pick_next → (空或同则落 idle 或不切)→ 同步 `current_` / `g_per_cpu.current` / `current_slice_` / (非 idle 才)`tss_set_rsp0` → `context_switch`。
4. `init()` 造出 `idle_task`(入口 `hlt`、priority 255、state Ready),且**不进就绪队列**;`schedule`/`exit_current` 在 `pick_next` 返回空时落到它,而非 `cli; hlt` 停机。
5. `context_switch.S` 在换栈之后、`jmp` 之前补了 `sti`;串口能看到多个线程被时钟中断交错打断,而非顺序跑完。
6. `GDT::tss_set_rsp0(uint64_t)` 写 `g_gdt.tss_.rsp[0]`,`run_first`/`schedule`/`exit_current` 切到非 idle 任务时都调它(并如实知道当前 ring0 下它未真正触发硬件换栈)。
7. `per_cpu.hpp` 提供 `PerCPU{current, kernel_stack}` + 全局 `g_per_cpu` 占位;`sync.hpp` 定义了 `Spinlock`(acquire/release/guard),但全内核无任何调用方。

七条都达成,内核就从「线程自觉才让 CPU」升级成「时钟到点强行换人」。但下一站 021 的钩子也在这里:调度器、就绪队列、PIT 这一路现在**全都没加锁**——单核 + 时钟中断这种「自上而下的抢占」下暂时没炸,可一旦真有多个执行流并发碰同一份数据(多核、或中断嵌套),竞态立刻浮上来。021 会基于这一关定义好的 `Spinlock` 去做 Mutex/Semaphore + 等待队列,让 `block`/`unblock` 真正派上用场,并立刻审查现有组件的并发安全性。再往后才是 `PerCPU` 从「单核全局」长成「真 per-CPU」、ring3 用户态与系统调用。
