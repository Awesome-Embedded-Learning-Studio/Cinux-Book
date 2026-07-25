---
title: 02 · 代码路线:从 CpuContext 到 Scheduler
---

# 代码路线:从 CpuContext 到 Scheduler

## CpuContext:64 字节的执行流快照

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

## context_switch.S:换栈,就是切换

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

## TaskBuilder.build:第一次切换,和以后的不一样

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

## RoundRobin + Scheduler:谁下一个

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

## higher-half 收口:内核该待在高半区

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
