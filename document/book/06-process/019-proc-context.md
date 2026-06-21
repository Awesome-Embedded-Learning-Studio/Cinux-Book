---
title: 019 · 让内核长出第二条执行流:进程上下文
---

# 019 · 让内核长出第二条执行流:进程上下文

> 上一章(018)我们铺好了「地址空间」这块地基——能给每个未来的进程一套独立页表了,可生产路径里它只做了 `init_kernel` 一件事,一个实例都没人造。为什么?因为还缺一样东西:**执行流**。从 009 到 018,内核从头到尾只有一条执行流——`kernel_main` 顺着往下跑,跑到底。没有「切换」,没有「同时存在多个活动」。这一章就是把这件事补上:我们造出「任务(`Task`)」这个抽象,写一段 70 来行的汇编让 CPU 能在两条执行流之间瞬间跳来跳去(上下文切换),再搭一个最朴素的轮转调度器把它们串起来。做完,内核就能让两个线程交替着往串口打字——`thread_a` 打一行、`thread_b` 打一行,像两个人共用一台打印机。顺带还要收一个上一章埋下的雷:大内核其实一直跑在「错的」地址上,这一章把它扶正回 higher-half——这事直接关系到进程隔离能不能成立。

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

## 代码路线

### CpuContext:64 字节的执行流快照

[process.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/process.hpp) 里,`CpuContext` 的定义极其克制:

```cpp
struct alignas(16) CpuContext {
    uint64_t r15, r14, r13, r12, rbp, rbx;   // 6 个 callee-saved
    uint64_t rsp;                             // 栈顶
    uint64_t rip;                             // 下一条指令
};

static_assert(offsetof(CpuContext, r15) == 0,  "r15 at offset 0");
// ... 另外 7 个 offsetof 断言同理 (共 8 个, 锁死每个字段偏移); 下面再锁总大小
static_assert(sizeof(CpuContext) == 64, "CpuContext must be 64 bytes");
```

`alignas(16)` 不是装饰:这条结构体会被汇编按固定偏移读写,也经常被一次性拷贝,16 字节对齐既配合 SSE 之类的要求,也防止结构体里出现意外的填充(padding)把偏移打乱。后面那串 `static_assert` 才是命门——它让「C++ 这一侧的布局」和「汇编那一侧写死的 `0/8/16/.../56` 偏移」在**编译期**就被绑定。谁要是手滑在中间加了个字段,编译直接红,而不是等运行时切到一半寄存器全错、查三天。

`TaskState` 是个简单的枚举:`Running / Ready / Blocked / Dead`。如实说:019 只用到前两个和最后一个——`Ready`(在队列里等着)、`Running`(正占着 CPU)、`Dead`(已退场、待回收)。`Blocked` 这个值在这一章**定义了但没人用**,它是给以后「线程等 I/O / 等锁」留的坑。看到枚举里有它,不代表功能已经在了。

### context_switch.S:换栈,就是切换

这段汇编([context_switch.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/context_switch.S))按 System V 约定拿参数:`%rdi = from`,`%rsi = to`。它分三段。

第一段,把当前任务的现场存进 `from`:

```asm
context_switch:
    movq %r15, 0(%rdi)      # from->r15
    movq %r14, 8(%rdi)
    movq %r13, 16(%rdi)
    movq %r12, 24(%rdi)
    movq %rbp, 32(%rdi)
    movq %rbx, 40(%rdi)
    movq %rsp, 48(%rdi)             # 存当前栈顶

    leaq .restore(%rip), %rax       # 算出"恢复点"的地址
    movq %rax, 56(%rdi)             # 把它当成 from->rip 存下来
```

最后那两行是整段的巧思。我们存 `from->rip` 时,存的不是「当前真正的下一条指令」,而是 `.restore` 这个标号的地址。含义是:**「当这个任务将来被切回来时,请从 `.restore` 那里继续。」** 这样一来,`context_switch` 对调用者来说就「像普通函数一样会返回」——只不过返回可能发生在很久以后、并且在它自己的栈上。

第二段,从 `to` 把现场恢复出来,并换栈:

```asm
    movq 0(%rsi), %r15      # to->r15
    movq 8(%rsi), %r14
    movq 16(%rsi), %r13
    movq 24(%rsi), %r12
    movq 32(%rsi), %rbp
    movq 40(%rsi), %rbx
    movq 48(%rsi), %rsp     # ← 换栈: 从这一刻起, 跑在 to 的栈上
```

`movq 48(%rsi), %rsp` 这一行就是「切换」本身。CPU 的执行流,说到底就是「一段栈 + 一个 rip」。把 rsp 换成 `to` 的栈顶,这条流的整个调用链就变成了 `to` 的调用链;后面所有的 `ret`、所有的局部变量,全在 `to` 的栈上发生。

第三段,跳过去:

```asm
    jmp *56(%rsi)           # 跳到 to->rip
.restore:
    ret                     # 切回来时从这里继续, ret 回到调用者
```

为什么是 `jmp` 不是 `call`?因为我们已经亲手把 `to->rip` 准备好了,不需要 `call` 再往栈上压返回地址——那条返回地址我们自己管(下面 `TaskBuilder` 会压 `exit_current`)。`jmp *56(%rsi)` 一跳,要么进了一条全新线程的入口,要么落到了某个任务当初存下的 `.restore`——后者会执行 `ret`,干净利落地「返回」到当初调用 `context_switch` 的地方(`yield` / `run_first`),仿佛这个函数刚执行完一样,只是栈和时机都变了。

### TaskBuilder.build:第一次切换,和以后的不一样

`context_switch` 跳到 `to->rip`。这就引出一个问题:一个**全新**的任务,它的 `ctx.rip` 该是什么?它的栈上又该有什么?答案藏在 [process.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/process.cpp) 的 `build()` 里——这是 019 最精妙也最容易写错的一段:

```cpp
Task* TaskBuilder::build() {
    // ... 从堆 new 出 TCB、从 PMM 要 4 页(16 KB)做内核栈、把栈映射进高半区 ...
    *reinterpret_cast<uint64_t*>(stack_virt) = STACK_MAGIC;   // 栈底写 0xDEADC0DE (溢出哨兵)

    task->ctx.rsp = stack_virt + stack_size - 8;              // rsp 指向"栈顶 - 8"
    *reinterpret_cast<uint64_t*>(task->ctx.rsp) =
        reinterpret_cast<uint64_t>(&Scheduler::exit_current); // ← 在栈顶压 exit_current
    task->ctx.rip = reinterpret_cast<uint64_t>(entry_);       // rip = 线程入口
    task->ctx.r15 = task->ctx.r14 = ... = task->ctx.rbx = 0;  // callee-saved 清零
    // ...
}
```

把这段拆成「它制造了什么效果」来看。当一个全新任务第一次被 `context_switch` 切到时:汇编恢复它全 0 的 callee-saved、把 rsp 设成它的栈顶、`jmp entry_`——于是线程函数从头开始跑,寄存器干干净净。这没问题。

妙的是**线程函数 `return` 之后会发生什么**。`context_switch` 是用 `jmp` 跳进 `entry_` 的,不是 `call`——所以线程函数的栈帧底下,没有一个「正常的返回地址」。如果什么都不做,线程函数一 `return`,`ret` 就会弹出栈顶那个值当返回地址;而栈顶此刻是我们**故意压在那儿的 `exit_current` 的地址**。于是 `return → ret → 跳进 exit_current()`——线程干完活,自动走进调度器的退场流程,把自己标记为 `Dead`、移出队列、切给下一个。这个「栈顶压 `exit_current`」的小动作,是协作式线程能**干净退出**的关键。

对比一下两种任务的 `ctx.rip`:

- **全新任务**:`rip = entry_`(线程函数),`rsp` 指向压了 `exit_current` 的干净栈。第一次切换 → 从头跑线程。
- **被打断过的任务**:它上次被切走时,汇编把 `.restore` 存进了它的 `rip`。再切回来 → 跳到 `.restore` → `ret` → 回到它当初调用 `yield` 的地方继续。

同一个 `context_switch`,靠 `to->rip` 里存的是什么,自动区分「第一次启动」和「恢复执行」——这就是 019 上下文切换的二元性。栈底那个 `0xDEADC0DE` 不是装饰:它是栈溢出哨兵,如果某个线程把栈用爆了,这个 magic 会被改写,以后能据此报警。调试现场里你会看到它**另一种**意外出场方式。

### RoundRobin + Scheduler:谁下一个

[scheduler.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/scheduler.cpp) 里,`RoundRobin` 是个定长环形队列(64 槽)。它的 `pick_next` 有个值得看清的细节:

```cpp
Task* RoundRobin::pick_next() {
    if (count_ == 0) return nullptr;
    Task* task = run_queue_[head_];
    head_ = (head_ + 1) % MAX_TASKS;   count_--;        // 出队头
    task->state = TaskState::Running;
    run_queue_[tail_] = task;
    tail_ = (tail_ + 1) % MAX_TASKS;   count_++;        // 又塞回队尾
    return task;
}
```

它**出队头之后,立刻把同一个任务塞回队尾**——这就是「轮转」:被选中的任务轮到队尾排队,下一圈再轮到它。返回的任务此刻状态是 `Running`,但还在队列里(在队尾)。

`Scheduler` 是个静态门面,把上面这些粘起来。三个关键入口:

```cpp
void Scheduler::yield() {          // 协作式: 线程主动让出
    Task* next = current_->sched_class->pick_next();
    if (next == nullptr || next == current_) return;   // 没别人, 不切
    Task* prev = current_;
    current_ = next;
    context_switch(&prev->ctx, &next->ctx);
}

void Scheduler::exit_current() {   // 线程 return 后走到这里
    Task* prev = current_;                         // 先存!
    prev->state = TaskState::Dead;
    prev->sched_class->dequeue(prev);              // 彻底移出队列
    Task* next = default_rr_.pick_next();
    if (next == nullptr) {                         // 队列空: 没人可切
        kprintf("[SCHED] No more tasks, halting.\n");
        while (1) __asm__ volatile("cli; hlt");    // 永久停机, 不返回
    }
    current_ = next;                               // 仅 next != nullptr 时走到这
    context_switch(&prev->ctx, &next->ctx);        // from != to
}
```

`exit_current` 第一行 `Task* prev = current_;` 看着多余,其实是上一版 bug 的直接修复——调试现场会讲:如果先 `current_ = next` 再切换,`from` 和 `to` 就指向同一个任务,`context_switch` 变成空操作,执行继续停在已经死亡的线程栈上,最终炸成 `0xDEADC0DE`。先存 `prev`,保证 `from != to`。

`run_first` 是引导:它拿一个**栈上的临时 `boot_task`**(tid=0,从不入队)当起点,`pick_next` 取出第一个真任务,切过去。从此 CPU 再也不回到这个 `boot_task`——它只是个跳板。

### higher-half 收口:内核该待在高半区

最后这一块不是「新功能」,是「把上一章埋的雷拆了」。看 [elf_loader.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/elf_loader.cpp) 末尾,019 之前是这么返回入口的:

```cpp
// 旧(错): 把 higher-half 入口剥回物理地址
constexpr uint64_t HIGHER_HALF_BASE = 0xFFFFFFFF80000000ULL;
uint64_t entry = saved_entry;
if (entry >= HIGHER_HALF_BASE) entry = entry - HIGHER_HALF_BASE;   // 0xFFFFFFFF81000000 → 0x1000000
return entry;

// 019(对): 直接返回链接时的 higher-half 入口
return saved_entry;
```

得结合 [linker.ld](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/linker.ld) 才看得懂为什么这是 bug:

```text
   KERNEL_VMA = 0xFFFFFFFF80000000   (higher-half 虚拟基址)
   KERNEL_LMA = 0x1000000            (物理加载地址, 16 MB)
   . = KERNEL_VMA + KERNEL_LMA        → 内核 .text 链接在 0xFFFFFFFF81000000
```

大内核是按 higher-half 地址 `0xFFFFFFFF81000000` **链接**的——它内部所有符号地址、所有绝对地址引用,都指望自己跑在这个地址上。可旧的 ELF 加载器把入口剥成了 `0x1000000`,然后 mini-kernel 跳过去。这之所以「能跑」,纯粹是因为引导加载程序顺手建了一条**恒等映射**(`PML4[0]` → 物理,盖住 `0x1000000`),让 `0x1000000` 和 `0xFFFFFFFF81000000` 指向同一片物理页。

但这件事和 018 的地址空间设计**正面冲突**。回忆 018:`AddressSpace` 的设计是「内核半区 `PML4[256..511]` 跨所有空间共享,用户半区 `PML4[0..255]` 每个空间私有」。内核理应待在**共享的**高半区,这样无论切到哪个地址空间,内核映射都在。可旧的加载器让内核跑在 `PML4[0]`(恒等映射,落在**用户半区**)——这正是每个地址空间各自私有、要重新建的那一半。于是麻烦来了:一旦开始给不同进程造独立地址空间,内核待在「本该私有」的那一半里,页表子树就被多个空间错误地共享,一个空间里建的页表项会顺着共享的 PDPT 子树**泄漏**到别的空间——进程隔离形同虚设。019 的调试笔记 `001_higher_half_fix` 记录了这条症状。

修复就一句:`return saved_entry;`,让内核回到它链接的 higher-half 地址,待在共享的高半区——隔离的地基这才算稳。(顺带一提,[main.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/main.cpp) 里那行 `[BIG] Big kernel running @ 0x1000000` 是个**遗留字符串**,它打的是物理基址,不代表修复后的运行地址;别被它误导以为内核还跑在 `0x1000000`。)

同一次收口里,还有两处配套小修。一是缺页处理 [exception_handlers.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/exception_handlers.cpp) 的 `handle_pf`:以前需求分页调 `g_vmm.map(virt, phys, flags)`,默认映射进**内核** PML4;现在先 `read_cr3()` 拿到当前地址空间的 PML4,把 `&cur_cr3` 传进去,让缺页页落在**当前**空间里(否则一旦真有多地址空间,缺页修错了地方),而且映射失败时会 `free_page` 把物理页还回去(修了个小泄漏)。二是 [vmm.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mm/vmm.cpp) 的 `walk_level` 多了**大页拆分**:当要下钻的区域恰好被一张 2 MB 大页盖住、而我们想要 4 KB 粒度时(比如给任务栈映射),它分配一张新页表,把那 2 MB 拆成 512 个 4 KB 项,再用新页表替掉大页项。这两处都是「为多地址空间铺路」的零碎活,019 顺手做了。

## 调试现场

这一章有两份真实笔记,都值得当案例,因为它们都是「看起来能跑、实际埋雷」的典型。

### 案例一:线程退出炸成 `0xDEADC0DE`

生产里两个线程交替打 5 轮后崩溃,串口吐出 `RIP=00000000deadc0de`,然后三重错误重启。根因是**两个 bug 叠加**。

其一是 `TaskBuilder` 当初没在栈顶压返回地址——线程函数 `return` 时 `ret` 弹空栈,一路弹到栈底那个 `0xDEADC0DE` 哨兵,把它当地址跳过去,CPU 跳到 `0xDEADC0DE` 当然炸。这正好解释了为什么现在 `build()` 里非要有那句「栈顶压 `exit_current`」:它就是给线程函数的 `return` 准备的落脚点。修复一:栈顶压 `exit_current`。

其二是就算修了第一个 bug,`exit_current` 本身还有毛病:它**先**把 `current_` 改成下一个任务,**再**调 `context_switch(&current_->ctx, &next->ctx)`——可这时 `current_` 已经是 `next` 了,`from` 和 `to` 指向同一个任务,切换成了空操作,执行继续停在已经死亡的线程栈上,该崩还是崩。修复二:进 `exit_current` 第一件事先 `Task* prev = current_;`,用 `prev` 当 `from`,保证 `from != to`。两个 bug 都修,两个线程才能干净利落地各打 5 轮、各打一句 `done`、各自 `[SCHED] ... exited`,最后队列空了打 `No more tasks, halting.`。

### 案例二:higher-half 没扶正,进程隔离失效

上面代码路线讲过:旧加载器剥掉 higher-half 偏移,让内核跑在恒等映射的 `PML4[0]`(用户半区),破坏了「内核待在共享高半区」的设计,导致地址空间之间页表子树泄漏。这条 bug 在 019「只有内核线程、还没真造多个用户地址空间」时不会立刻发作——因为演示的两个线程共享内核地址空间。但它是颗定时炸弹:一旦 020 之后真给进程造独立地址空间,这颗雷就会以「A 进程的页表项莫名出现在 B 进程里」的形态爆出来。019 把加载器扶正,等于在雷爆之前拆了引信。这种「当下不发作、但迟早要命」的 bug,是做内核时最值得记进笔记的一类。

## 验证

调度逻辑(队列轮转、入队出队、`CpuContext` 布局、`TaskBuilder` 字段)在 host 上镜像着测。[test_scheduler.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_scheduler.cpp) 把 `RoundRobin`、`Scheduler`、`TaskBuilder`、`CpuContext` 的逻辑在 host 侧重写了一份(不链内核代码,`-O2` 编、`CINUX_HOST_TEST` 门控),盯这些:空/满/单任务队列的 `pick_next`、`dequeue` 中间项、`TaskBuilder` 的字段默认值与 null entry 守卫、`CpuContext` 的 `sizeof` 和各偏移:

```bash
ctest --test-dir build -R scheduler --output-on-failure
```

真正的 `context_switch`(真汇编换栈)和真正的任务构造(真 PMM/VMM/Heap 出栈)只能在 QEMU 里验。[test_scheduler.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_scheduler.cpp) 在机内跑一连串场景:`TaskBuilder` 能造出合法任务(tid 从 1 起、state=Ready、`ctx.rip` 指向入口、栈非 0——这一项用的是真 PMM/VMM/Heap 建出来的栈),null entry 返回 nullptr,`init` 注册默认 `RoundRobin`,`RoundRobin` 的 enqueue/dequeue/pick_next 轮转与出队中间项,`CpuContext` 布局(`sizeof==64`、各 offset);而专门验 `context_switch` 本身的那一项用的是两个**裸 `CpuContext` + 静态栈缓冲区**(不经过 `TaskBuilder`),纯测汇编换栈能不能让两个上下文来回切、状态在 Ready/Running 间正确流转:

```bash
cmake --build build --target run-big-kernel-test
```

机内会打 `[SCHED] Scheduler initialised with RoundRobin class`,test section `Scheduler/Process Tests (019)` 全过、末尾 `ALL TESTS PASSED`,就说明这套任务/切换/调度在真硬件语义下成立。

最后是**生产 demo** 本身的现象:直接跑大内核(`cmake --build build --target run`,或对应 QEMU 目标),串口应该看到 `thread_a` / `thread_b` 严格交替的 5 轮、各自一句 `done`、各自 `[SCHED] Task tid=N '...' exited`,最后 `No more tasks, halting.`——交替说明 `context_switch` 真的把 CPU 在两条流之间递来递去,干净退出说明「栈顶压 `exit_current`」那条设计是对的。

## 下一站

到这里,内核第一次有了「多条活着的执行流」。`Task` 把一条执行流的所有可挂起状态装进 64 字节;`context_switch.S` 用「存 callee-saved + 换栈 + 跳 rip」在它们之间瞬切;`RoundRobin` 轮流点名;higher-half 扶正让隔离地基稳了。

但你会发现 019 的痛:它是**协作式**的。线程要是不主动 `yield`,它就霸着 CPU 不放——`thread_a` 如果忘了调 `yield`,`thread_b` 永远没机会跑。真实的系统不能指望每个线程都自觉。下一站(020)就治这个:把调度器接到那个**已经在跑**的时钟中断上,让 `irq0` handler 在固定节拍打断当前线程、强制切走——也就是**抢占式**调度。那会引入新的难题(中断可以在任意指令处发生,不再是干净的函数边界;切走时要保存的现场更重;多个 CPU 各自的当前任务怎么管),于是 020 还会带来 `per_cpu` 和最基本的同步原语。019 的 `Task` 和 `context_switch` 是那一切的地基——地基先稳,上面才好盖。

---

### 参考

- **System V AMD64 ABI**([x86-psABIs/x86-64-ABI](https://gitlab.com/x86-psABIs/x86-64-ABI)):callee-saved 寄存器约定(`rbx`、`rbp`、`r12`–`r15`)——`CpuContext` 只存这 6 个 + `rsp`/`rip` 的全部依据;caller-saved 跨调用不保证存活,故无需保存。
- **xv6-riscv `swtch()`**([mit-pdos/xv6-riscv](https://github.com/mit-pdos/xv6-riscv)):同样的「只存 callee-saved + 换栈 + ret」手法,可对照 Cinux `context_switch` 的设计。
- **Intel SDM Vol.3**(本地 `document/reference/intel/SDM-Vol3A-*.pdf`):通用寄存器集、`rip`/`rsp` 如何定义执行流、规范地址(higher-half 的由来),可用 `pdf-reader` 搜 "general-purpose" / "canonical" 复核。
- 018 章 · [给每个世界一套页表:地址空间](../05-memory/018-mm-address-space.md):`AddressSpace` 的「内核半区 `PML4[256..511]` 共享、用户半区私有」设计——higher-half 收口之所以必要,就是为了对上这个设计。
- 本 tag 源码:[process.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/process.hpp) / [process.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/process.cpp)、[context_switch.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/context_switch.S)、[scheduler.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/scheduler.hpp) / [scheduler.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/scheduler.cpp)、[elf_loader.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/elf_loader.cpp)(`return saved_entry`)、[linker.ld](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/linker.ld)(`KERNEL_VMA`/`KERNEL_LMA`);测试 [test_scheduler.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_scheduler.cpp)(host 镜像)、[test_scheduler.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_scheduler.cpp)(QEMU 机内)。
