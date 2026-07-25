---
title: 04 · band 0 kthread 不能 sti/hlt:yield 解法(本章明星,串 071)
---

# band 0 kthread 不能 sti/hlt:yield 解法(本章明星,串 071)

这是本章最硬的反直觉真坑。源码头注释([stats_kthread.cpp:12-24](../../../kernel/mm/stats_kthread.cpp#L12))把三版尝试讲成一条推理链,咱们逐段拆。

**前提**:Cinux 调度器优先级是「lower runs first」([scheduler.hpp:41](../../../kernel/proc/scheduler.hpp#L41) 头注释:「priority-aware selection (lower Task->priority runs first)」)。`TaskBuilder` 默认 `priority_ = 0`([task_builder.hpp:81](../../../kernel/proc/task_builder.hpp#L81)),被 `/sbin/init` 继承,被 fork/exec 出来的 cc1 继承——所以全系统用户代码都坐 band 0。stats kthread 要采的就是这些 band 0 用户的 workload。

那 stats kthread 自己该坐哪一档?三个候选,前两个各自炸在一个不同维度。

## 症状一:priority 250(近 idle 255)+ yield → 观察者饿死在被观察者手里

第一版尝试给 stats kthread 一个低优先级(比如 250,接近 idle 的 255),心想「它是个后台观测线程,跑得闲一点就行」。

- **症状**:开 `CINUX_STATS_KTHREAD=ON`、make run、跑 `g++ hello.cpp`,串口只有 `[MEM] stats thread entry running` 那一行启动日志,**整个编译期 0 行 dump**。
- **根因**:「lower runs first」——只要还有任何 band 0 任务 Ready,CPU 就先跑 band 0,band 250 的 stats kthread 永远轮不到。而 `g++ hello.cpp` 恰恰是个 CPU-bound workload——cc1/cc1plus 几乎全程占着 CPU。结果是「**观察者饿死在被观察者手里**」——你专门为了观察编译卡顿起的线程,被编译本身饿死了,一行采样都采不到。
- **定位**:看串口 dump 行数——启动后到编译结束,`[MEM] === t=...` 那行一次都没出。
- **修复**:提到 band 0,和被观测对象同档。
- **防复发**:头注释([stats_kthread.cpp:16-18](../../../kernel/mm/stats_kthread.cpp#L16))写明「Lower (e.g. 250, near idle 255) STARVES under a CPU-bound compile -- the exact workload we want to observe -- so no samples ever land」。

## 症状二:priority 提高 → 抢占 user code,自欺欺人

第二个候选是反过来——给 stats kthread 一个**比 user code 更高**的优先级(数字比 0 还小,或抢在 band 0 前),心想「让它优先跑,采样及时」。

这条没真在 Cinux 上跑过(头注释是一句话带过的「Higher would preempt user code and skew the very numbers we measure」),但逻辑很清楚:

- **根因**:你专门起一个线程去量 user code 的内存行为,结果这个线程自己优先级比 user code 高——user code 没跑满就被你抢占、你 `dump_memory_stats` 时 user code 没在干活——你采的是「**被你干扰过的数**」,自欺欺人。这违反了观测的基本伦理:**观测者不能干扰被观测对象**。量子力学的测不准原理在性能剖析里有一个朴素的工程版本——你的采样线程不能扭曲它要量的东西。
- **修复**:不能更高,必须和 user code 同档。

所以 stats kthread 必须 band 0——既不能低(饿死)、也不能高(扭曲)。但 band 0 内「等 1 秒」还有第三个坑。

## 症状三:band 0 + sti/hlt → gate 卡在第一次 fork(本章最坑)

第三版尝试是 band 0,但「等 1 秒」用 `sti; hlt`(开中断然后 halt 等 IRQ)——心想「halt 省 CPU,tick IRQ 唤醒自己」。

- **症状**:`CINUX_STATS_KTHREAD=ON` make run,系统启动到 busybox init 第一次 fork 就**卡死**——串口停在 init fork 那条日志,#PF 全程 +0(根本没采到样)。
- **根因**:`hlt` 在一个 band-0 线程里是灾难。`hlt` 让 CPU 停下来等中断,IRQ0(PIT tick)来了唤醒——但唤醒后**当前 quantum 没耗尽**,调度器 tick 里 `task_tick` 只记账不强制切换(注释 [scheduler.cpp:381-385](../../../kernel/proc/scheduler.cpp#L381) 写明「context_switch.S enables IF before jumping to the next task; doing that while the old irq0 frame is still live lets the PIT re-enter recursively. Real preemption needs a return-from-IRQ resched point」——这条注释确立的是「tick 不内联切」这个非抢占模型;而「hlt 后的线程会被反复选中」这个具体后果是 CinuxOS dev note 定位的:`stats 在时间片内 hlt,tick IRQ 唤醒后继续 stats(时间片未耗尽),init/child 永远 Ready 等 → 饿死`),于是 CPU 又选回 priority 0 最高的 stats kthread 自己——它再 `hlt`、再被 tick 唤醒、再选自己......init / fork 出来的 child 永远是 Ready 状态等不到 CPU。这跟 071 章那个 sti/hlt #DF 坑是**同族**(都是 Cinux 里 sti/hlt 用错上下文),但**机制不同**:071 章是 syscall 上下文里 `sti` 打开一个窗口,LAPIC 时钟中断在那个窗口抢 `%gs:0` 栈上的 syscall 陷阱帧,sysretq 弹花就 #DF(根子是**陷阱帧损坏**);这里是 band-0 kthread 里 `hlt`,tick IRQ 唤醒后 quantum 没耗尽,调度器重选自己,把同级 init/child 饿死(根子是**非抢占 + 同优先级重选**)。两者的共性只到「都是在错误上下文用 sti/hlt」这一层,不要把两个不同的根机当成同一个。
- **定位**:看 #PF 卡在 +0、gate 卡在 busybox init 第一次 fork——典型的「观测线程垄断了 CPU、被观测对象跑不动」。
- **修复**:不 `hlt`,改 `yield`。
- **防复发**:头注释([stats_kthread.cpp:19-22](../../../kernel/mm/stats_kthread.cpp#L19))明确「do NOT sti/hlt: halting inside a band-0 thread makes the tick IRQ resume us (quantum not exhausted) and every other band-0 task (init / fork / exec) waits forever -- the gate freezes at the first fork with #PF stuck at +0」。

## 正解:band 0 + yield——醒得很勤,干活很稀

正解是 band 0(priority 0)+ `yield`:

```cpp
// kernel/mm/stats_kthread.cpp:54-59(主循环开头)
while (true) {
    // yield() hands the CPU to the next band-0 task (init/cc1/g++); the PIT
    // tick preempts it back to us, so we wake ~once per tick (~10 ms) --
    // far below the 1 s dump interval.  See file header for why this is NOT
    // sti/hlt and NOT a lower priority.
    cinux::proc::Scheduler::yield();
    // ... 查 HPET deadline,到了才 dump ...
}
```

`yield` 主动把 CPU 让给下一个 band-0 task(init/cc1/g++)——它干活,你睡;PIT tick(~10ms 后)再切回 stats,你醒一次查一次 HPET deadline(几 ns 的活儿),没到 1 秒就回去再 yield。这是「**醒得很勤(约 100Hz)但干活很稀(1Hz dump)**」的两级节流:

- 不饿死编译——yield 让出 CPU 给 cc1,cc1 真在跑。
- 不扭曲测量——yield 是协作式让出,你 dump 的瞬间 user code 刚跑完一个 quantum,采的是真实的 workload 状态。
- 不垄断 CPU——yield 不 halt,不把 CPU 交给中断反复选自己。

`yield` 在这里同时干两件事:**让出 CPU 给被观测对象** + **等下一个 tick**。一个原语两个职责,这就是为什么 stats_kthread 没用 timer_queue——timer_queue 的 park 会把 task 翻 Blocked 睡死,错过中间所有的采样窗口(它要的是「醒 100 次只 dump 1 次」,不是「睡 1 秒醒 1 次」);而且 park 的 task 在 band 0 里同样会撞上「谁来叫醒它」的问题——叫醒它的 tick IRQ 在 band 0 上下文,timer_queue 的 `unblock` 把它翻 Ready 后,调度器选谁还是 priority 说了算。所以 stats_kthread 是**另一种定时范式**:不是「到点叫醒」(timer_queue),而是「勤醒懒干活」(yield + 查 deadline)。

教学点:在协作式/带优先级的调度器里,后台观测线程的调度不是随便挑个原语——它必须够高优先级(不饿死)、不能更高(不扭曲)、且不能 halt(不垄断剩余时间片)。`yield` 是唯一同时满足这三个约束的姿势。
