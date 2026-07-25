---
title: 02 · 代码路线:时钟驱动抢占与配套地基
---

# 代码路线:时钟驱动抢占与配套地基

## tick 与 schedule:让时钟来点名

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

## context_switch.S 的 sti:从中断上下文切出去,必须把中断打开

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

## idle 任务:队列空了也有地方歇

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

## TSS.RSP0 与 GDT::tss_set_rsp0

[gdt.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/gdt.cpp) 新增一个直写的静态方法:

```cpp
void GDT::tss_set_rsp0(uint64_t rsp0) {
    g_gdt.tss_.rsp[0] = rsp0;   // 直接写 TSS 里 ring0 的栈顶槽
}
```

`TSS` 结构体([gdt.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/gdt.hpp))按 SDM Vol.3A Figure 8-11 / §8.7「Task Management in 64-bit Mode」摆好 104 字节(源码注释里把它标成「Table 8-2」,但 SDM 实际以 Figure 8-11 呈现这张表),`rsp[3]` 是三个特权级的栈顶(ring0 用 `rsp[0]`)。每次 `run_first` / `schedule` / `exit_current` 切到非 idle 任务,都调一次 `tss_set_rsp0(next->kernel_stack_top)`。

为什么要这么干?SDM §6.12.1 说:当 handler 要在**更低特权级**(数值更大,即 ring3→ring0)执行时,处理器会从当前任务的 TSS 取 handler 要用的新栈顶(`SS:RSP`)。也就是说,RSP0 是「下一次从用户态掉进内核态时,硬件自动换上的那个内核栈顶」。既然每个任务有自己的内核栈,切到新任务时就得把 RSP0 指向新任务的内核栈顶,否则将来真有用户态进程时,缺页、系统调用掉进内核会用到**上一个任务**的内核栈,栈错位直接炸。

但必须如实说:020 全程是 ring0 内核线程,不发生任何特权级变化,所以这条 `tss_set_rsp0` **现在其实不会触发硬件换栈**——硬件压根没走到「从 TSS 取栈」那一步。它是个「接口先接上、等将来 ring3 来了再真正生效」的动作。写它、调它,是为了将来有用户进程时这块不用再回来补;不是因为它现在已经在保护什么。

## PerCPU 占位:为多核先挖个坑

[per_cpu.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/per_cpu.hpp) 整个文件就这么点东西:

```cpp
struct PerCPU {
    Task* current;          // 当前在跑的任务
    uint64_t kernel_stack;  // 内核栈顶(留给将来 RSP0 登记)
};

extern PerCPU g_per_cpu;    // scheduler.cpp 里定义: PerCPU g_per_cpu{nullptr, 0};
```

每次切换,`schedule` / `run_first` / `exit_current` 都同步一句 `g_per_cpu.current = next;`。得诚实讲清楚它**不是**什么:它不是 GS 基址相对寻址的真 per-CPU 区,也不是每 CPU 独立运行队列,就是一个**单核静态全局变量**。020 只有一个 CPU,放它纯粹是为了让「将来 `current` 从全局迁移到 per-CPU」时改动小——先把读取入口统一到 `g_per_cpu.current`,将来换成 GS 相对寻址时,只动这一个定义,调用点不用大改。别把它说成 SMP 地基,它现在连第二份实例都没有。

## sync.hpp:Spinlock 原语,先定义着

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
