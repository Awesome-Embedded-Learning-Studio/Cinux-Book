---
title: Lab 019 · 让内核长出第二条执行流:进程上下文
---

# Lab 019 · 让内核长出第二条执行流:进程上下文

> 配套章节:[019 · 让内核长出第二条执行流:进程上下文](../../book/06-process/019-proc-context.md)。这一关给你目标和约束,不贴 `context_switch.S` 的汇编、不贴 `TaskBuilder::build()` 的完整实现、不贴 `exit_current` 的修复——那些得你自己写、自己踩坑、自己修出来。

## 实验目标

让内核从「只有一条 `main` 流」升级成「能同时持有多个可切换的内核线程」,并让它们干净地交替、干净地退出。拆成几个能独立验证的子目标:

1. **定义执行流的快照**:一个只存 callee-saved + `rsp` + `rip` 的 `CpuContext`,偏移用 `static_assert` 锁死;一个 `TaskState` 枚举;一个装下它们的 `Task`(TCB)。
2. **写上下文切换**:一段汇编 `context_switch(from, to)`,存当前现场、恢复目标现场、换栈、跳到目标的 `rip`。要让调用者感觉它「像普通函数一样会返回」,但实际返回在目标任务的栈上、且可能在很久以后。
3. **造任务**:一个 `TaskBuilder`,负责从堆分配 TCB、从 PMM 要内核栈,并把任务的初始现场布置成「第一次切到时从头跑线程、线程 `return` 时干净退场」。
4. **搭调度器**:一个轮转队列 + 一个静态门面,提供 `init` / `add_task` / `yield` / `exit_current` / `run_first`。
5. **跑协作式 demo**:`main` 里造两个线程各打印若干轮、每轮手动 `yield`,验证严格交替与干净退出。

做完这几条,内核就有了「多条执行流并存并在它们之间瞬切」的能力——抢占、用户态、阻塞唤醒都还谈不上,但最底下的「切换」这一锤,砸实了。

## 前置条件

你得先过 Lab 015 / 016 / 017 / 018。关键依赖:

- **015 的 `g_pmm`**:`alloc_pages(n)` / `alloc_page()` / `free_page()`——任务的内核栈要从 PMM 要物理页。
- **016 的 `g_vmm.map(virt, phys, flags, uint64_t* pml4 = nullptr)`**:把任务的内核栈映射进内核虚拟地址空间。注意它带个可选的 `pml4` 根参数(018 用过)。
- **017 的内核堆**(`new` / `kmalloc`):TCB 这种小结构从堆分配。
- **018 的 higher-half 设计**:你得理解「内核映射在所有地址空间共享的高半区(`PML4[256..511]`),用户映射在各自私有的低半区」——任务栈要映射在高半区,这样无论将来切到哪个地址空间,栈都在。

还得理解一个外部约定:**System V AMD64 ABI** 的寄存器分类——`rbx/rbp/r12-r15` 是 callee-saved(跨调用必须保持),其余通用寄存器是 caller-saved(跨调用不保证)。这一关的 `CpuContext` 只存 callee-saved,根因就在这条约定。

## 任务分解

**第一步:`CpuContext` + `TaskState` + `Task`。** `CpuContext` 就 8 个 `uint64_t` 字段:`r15/r14/r13/r12/rbp/rbx/rsp/rip`,顺序和偏移就是你汇编里要用的(`0/8/.../56`)。给结构体 `alignas(16)`,然后用一串 `static_assert(offsetof(...) == N)` 把每个字段、以及 `sizeof(...) == 64`,在编译期钉死。想清楚为什么**只存这 8 个**(caller-saved 在函数调用边界本来就不保证存活,编译器自己管),存多了浪费、存少了破坏调用约定。`TaskState` 用个枚举:`Running/Ready/Blocked/Dead`(如实说,这一关 `Blocked` 定义了但用不上)。`Task` 里至少放:`CpuContext ctx`、`TaskState state`、`uint64_t tid`、栈基址与栈顶、任务名,以及几个「为以后留」的字段(优先级、地址空间指针、调度类指针——这一关可以先留空/默认值)。

**第二步:`context_switch.S`(最难,也最值)。** 按 System V 约定,`%rdi = from`、`%rsi = to`。三段:(a) 把当前 callee-saved(`r15..rbx`)、当前 `rsp` 存进 `from` 的对应偏移;再算出**本函数末尾「恢复点」标号**的地址,把它存进 `from->rip`——这一步要想清楚为什么存的是「恢复点」而不是「真正的下一条指令」(答:让被切走的任务将来回来时,能像 `context_switch` 正常返回一样,`ret` 回到它当初调用 `yield` 的地方)。(b) 从 `to` 把 callee-saved 恢复回寄存器,再把 `to->rsp` 装进 `rsp`——**换栈这一行就是「切换」本身**。(c) `jmp *to->rip`(不是 `call`!因为你已经亲手备好了 `rip`,不需要再压返回地址)。在「恢复点」标号处放一条 `ret`。这一关**不给汇编全文**,但给你三条不可破的约束:偏移必须和 `CpuContext` 的 `static_assert` 对得上;必须先存完 `from` 再动 `rsp`,否则你存进 `from` 的就不是当前任务的真栈顶;末尾必须是 `jmp` 不是 `call`。

**第三步:`TaskBuilder::build`(第二个雷区)。** 流式 set 完入口/名字/优先级后,`build()` 干这些:从堆 `new` 出 `Task` 并清零;从 PMM 要 N 页(比如 4 页 = 16 KB)做内核栈,逐页 `g_vmm.map` 进高半区的某个虚拟地址;栈底写一个溢出哨兵 magic(比如 `0xDEADC0DE`);然后是**最关键的两行**——把任务的 `ctx.rsp` 设成「栈顶 - 8」,并在那个位置写入**调度器退场函数的地址**,再把 `ctx.rip` 设成线程入口,callee-saved 清零。想清楚这两行制造的效果:任务第一次被切到时,汇编恢复它全 0 的寄存器、把 `rsp` 设成这个栈顶、`jmp` 线程入口 → 线程从头跑;线程函数 `return` 时,`ret` 弹出栈顶那个值——而那个值是你故意压的退场函数地址 → 线程干净走进退场流程。**如果不压这个地址,线程 `return` 时 `ret` 弹空栈,一路弹到栈底那个 `0xDEADC0DE` 哨兵,把它当地址跳过去,CPU 当场炸**(这是这一关最常见的翻车点)。退场函数的地址怎么拿到?它是 `Scheduler` 的一个静态成员函数,`build()` 在 `Scheduler` 定义可见之后才能取它的地址——注意头文件依赖顺序。

**第四步:`RoundRobin` + `Scheduler`。** `RoundRobin` 是个定长环形数组(比如 64 槽)+ head/tail/count。`enqueue` 尾插、`dequeue` 找到指定任务删掉(删中间项要前移后续元素)、`pick_next` 有个要特别想清楚的细节:**出队头之后,把同一个任务再塞回队尾**(这才叫「轮转」,否则就是「先来先服务、跑完出队」)。`Scheduler` 是静态门面:`init` 注册默认 `RoundRobin`;`add_task` 把任务塞进它的调度类(没指定就用默认 RR);`yield` 选下一个、若与当前相同则不切、否则 `context_switch(prev, next)`;`exit_current` 是第二个雷区(见约束);`run_first` 用一个栈上的临时 `boot_task` 当跳板,`pick_next` 取第一个真任务切过去。想清楚 `boot_task` 为什么**不入队**(它只是起点,不是要被调度的任务)。

**第五步:`main` 里的协作式 demo。** `Scheduler::init()`,用 `TaskBuilder` 造两个线程(各 `set_entry`/`set_name`,`build`),`add_task` 进去,然后 `run_first(&boot_task)`。两个线程函数各 `for` 打印若干轮,每轮末尾 `Scheduler::yield()`。预期串口看到两个线程**严格交替**输出、各自一句 done、各自一条退场日志,最后队列空了打「没任务了」。

## 接口约束

你要实现出来的东西,对外长这样(职责与签名,不给实现):

- `enum class TaskState : uint8_t { Running, Ready, Blocked, Dead };`
- `struct alignas(16) CpuContext { uint64_t r15, r14, r13, r12, rbp, rbx, rsp, rip; };`(偏移用 `static_assert` 锁死,`sizeof == 64`。)
- `struct Task { CpuContext ctx; TaskState state; uint64_t tid; ...; const char* name; SchedulingClass* sched_class; };`
- `class TaskBuilder`:`set_entry(void(*)()) / set_name / set_priority / ... / Task* build();`(`build` 前必须 `set_entry`,否则返回 nullptr。)
- `extern "C" void context_switch(CpuContext* from, CpuContext* to);`(汇编实现。)
- `class SchedulingClass`(抽象:`enqueue/dequeue/pick_next/name`)、`class RoundRobin : public SchedulingClass`、`class Scheduler`(静态:`init/register_class/add_task/yield/exit_current/run_first/current`)。

关键约束(违反就翻车):

- **`CpuContext` 的字段顺序/偏移,必须和 `context_switch.S` 里写死的偏移逐字节对上**——这是 C++ 与汇编的契约,用 `static_assert` 在 C++ 侧锁,汇编侧照抄同样的数字。错一位,切换时寄存器全乱。
- **`context_switch` 必须先存完 `from` 的现场、再动 `rsp`**。一旦先 `mov to->rsp, %rsp`,后面再 `mov %rsp, from->...` 存进 `from` 的就是 `to` 的栈顶,当前任务的现场永久丢失。
- **新任务的栈顶必须压退场函数地址**,否则线程 `return` 时炸成栈底哨兵 magic。
- **`exit_current` 必须先 `Task* prev = current_;` 再做任何覆盖**。若先把 `current_` 改成 `next` 再 `context_switch(&current_->ctx, &next->ctx)`,`from` 和 `to` 指向同一任务,切换成空操作,执行停在已死线程的栈上——必崩。
- **`exit_current` 在队列空时要安全收尾**(比如 `cli; hlt`),不能返回到一个不存在的任务。

汇编里偏移具体是几个字节、环形队列怎么用 head/tail 算下标、栈映射到哪个虚拟地址、哨兵 magic 取什么值——这些**这一关不提供**,你自己定,但定下来就要和 `static_assert`、和 `TaskBuilder` 对齐。

## 验证步骤

调度逻辑(队列轮转、入队出队、`CpuContext` 布局、`TaskBuilder` 字段与守卫)在 host 上镜像着测——把 `RoundRobin`/`Scheduler`/`TaskBuilder`/`CpuContext` 的纯逻辑在 host 侧重写一份(不链内核、不跑汇编),`-O2` 编、`CINUX_HOST_TEST` 门控。建议覆盖:空/满/单任务队列的 `pick_next`、`dequeue` 中间项、`pick_next` 的「出队头又塞回队尾」轮转行为、`TaskBuilder` 字段默认值、`build()` 对 null entry 返回 nullptr、`CpuContext` 的 `sizeof` 与各 `offsetof`:

```bash
ctest --test-dir build -R scheduler --output-on-failure
```

真正的 `context_switch`(真汇编换栈、真 PMM/VMM/Heap)只能在 QEMU 里验。机内测要覆盖:`TaskBuilder` 造出合法任务(tid 从 1 起、state=Ready、`ctx.rip` 指向入口、栈非 0 且栈顶在栈基之上)、null entry 返回 nullptr、`init` 注册默认 RR、`RoundRobin` 的 enqueue/dequeue/pick_next 轮转与出队中间项、`CpuContext` 布局,以及**两个真任务之间靠 `context_switch` 的协作切换**与生命周期 Ready→Running:

```bash
cmake --build build --target run-big-kernel-test
```

机内会打 `[SCHED] Scheduler initialised with RoundRobin class`,test section `Scheduler/Process Tests (019)` 全过、末尾 `ALL TESTS PASSED`。

最后跑**生产 demo** 本身(直接跑大内核的 QEMU 目标),串口应看到两个线程严格交替的若干轮、各自一句 done、各自退场日志,最后「没任务了」——交替证明 `context_switch` 真在两条流之间递 CPU,干净退出证明「栈顶压退场函数」是对的。

## 常见故障

- **线程跑完一轮就崩,串口吐 `RIP=00000000deadc0de`,三重错误重启**:`TaskBuilder` 没在新任务栈顶压退场函数地址。线程 `return` 时 `ret` 弹空栈,一路弹到栈底的 `0xDEADC0DE` 哨兵,跳过去就炸。修复:`build()` 里把 `ctx.rsp` 设成栈顶 - 8,在那里写退场函数地址。
- **修了上面那条还是崩,而且崩在「线程该退场」的时候**:`exit_current` 先把 `current_` 改成下一个任务、再 `context_switch(&current_->ctx, &next->ctx)`——这时 `from == to`,切换是空操作,执行继续停在已死线程栈上。修复:`exit_current` 第一行先 `Task* prev = current_;`,全程用 `prev` 当 `from`,保证 `from != to`。
- **切一次之后寄存器全乱、函数返回到莫名其妙的地址**:`CpuContext` 的字段顺序/偏移和 `context_switch.S` 里写死的偏移对不上(C++ 侧加了字段或改了顺序,汇编没跟着改)。用 `static_assert(offsetof(...))` 在编译期锁死,汇编照抄同样的数字。
- **切一次之后,某个任务再也回不来(或回来到错误的栈)**:`context_switch` 里先 `mov to->rsp, %rsp`(换栈)了,再 `mov %rsp, from->...`——存进 `from` 的是 `to` 的栈顶,当前任务现场丢失。必须**先存完 `from`,再动 `rsp`**。
- **`pick_next` 总返回同一个任务,或队列越跑越短**:轮转逻辑写成了「出队头、不塞回」,或者塞回的位置算错了。`pick_next` 应该出队头之后把同一任务塞回队尾(head 前进、tail 跟上,count 净不变)。
- **`yield` 之后没切走**:`yield` 没判断 `next == current_` 的情况,或队列里只有一个任务时强切导致空操作。队列里没别的任务就别切。
- **任务栈映射后访问就缺页**:栈虚拟地址没 `g_vmm.map` 成功(或映射到的页表层级因为大页挡路没建出来),或 `ctx.rsp` 算错了(没对齐、或指向了未映射的页)。栈要逐页映射、`rsp` 要落在已映射的栈顶。

## 通过标准

1. `CpuContext` 只存 callee-saved(`r15..rbx`)+ `rsp` + `rip`,偏移用 `static_assert` 锁死、`sizeof == 64`;字段顺序与汇编偏移逐字节对齐。
2. `context_switch.S`:先存完 `from` 现场再换栈;末尾 `jmp *to->rip`(非 `call`);恢复点标号处 `ret`;让被切走的任务将来能「正常返回」到当初的调用点。
3. `TaskBuilder::build`:从堆分配 TCB、从 PMM 要内核栈并映射进高半区、栈底写哨兵;**新任务栈顶压退场函数地址**、`ctx.rip = 入口`、callee-saved 清零;`set_entry` 缺失时返回 nullptr。
4. `RoundRobin.pick_next` 出队头后塞回队尾(真轮转);`Scheduler` 提供 `init/add_task/yield/exit_current/run_first`;`exit_current` 先存 `prev`、队列空时安全收尾。
5. host 单测全绿:队列轮转/出入队/布局/`TaskBuilder` 守卫;QEMU 机内测通过(含两个真任务的 `context_switch` 协作切换)。
6. 生产 demo:两个线程严格交替、各自干净退出,最后「没任务了」。

做到这六条,内核就第一次有了「多条可切换的执行流」。但它是协作式的——线程不 `yield` 就独占 CPU。谁来逼它让出?时钟中断。那是下一关 020 的事:把调度器接到那个**已经在跑**的 PIT 时钟中断上(019 的 `main` 里 `PIT::init(100)` + `sti` 之后,IRQ0 每个 tick 都在触发,只是 handler 还没碰调度器),让中断在固定节拍打断当前线程、强制切走,做成抢占式调度——那会带来 `per_cpu`、同步原语,以及「中断能在任意指令处发生」这个比函数边界难缠得多的问题。
