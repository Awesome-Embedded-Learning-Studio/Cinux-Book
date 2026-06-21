---
title: Lab 021 · 让任务睡得下去、醒得过来:内核同步原语
---

# Lab 021 · 让任务睡得下去、醒得过来:内核同步原语

> 配套章节:[021 · 让任务睡得下去、醒得过来:内核同步原语](../../book/06-process/021-proc-sync.md)。这一关给你目标和约束,不贴 `Mutex::lock()` 五步、不贴 `Semaphore::wait()` 的负 count 分支、不贴 `enqueue_waiter`/`dequeue_waiter` 的链表函数体、不贴 `build()` 全文——那些得你自己把「先释放自旋锁再 block」「unlock 直接把 owner 交给 waiter」这两条想明白,自己写出来、自己踩坑修出来。

## 实验目标

020 已经在调度器里铺好了 `block`/`unblock`——能把任务标成 `Blocked` 摘出就绪队列、又能把它唤醒塞回去。可铺好之后没人用:全内核仍然只有那六个空转的忙循环线程。这一关要做的事,就是把 `block`/`unblock` 接成真正的同步原语,并跑通一个生产者-消费者。拆成四块,缺一块都不算数:

1. **把 `Spinlock` 从头文件里搬出来**。020 那个 `Spinlock` 是内联在 `sync.hpp` 里的占位,这一关要让它真正用上 `__atomic_test_and_set` / `__atomic_clear`,配上 acquire/release 内存序,搬进 `sync.cpp`,成为 Mutex/Semaphore 的内部构件。
2. **实现 `Mutex`**。一把阻塞式互斥锁:有空闲就拿、竞争就进 FIFO 等待队列 `block` 睡下去;`unlock` 不清空 owner,而是把所有权直接交接给队首 waiter。
3. **实现 `Semaphore`**。一个允许 count 走到负数的计数信号量:负数的绝对值就是正在队列里阻塞的等待者数。
4. **把三件套拼成生产者-消费者**。一个 `Semaphore free(N)` 计空位、一个 `Semaphore used(0)` 计已填充、一把 `Mutex` 保护共享环形缓冲,producer/consumer 两个内核线程跑起来。

做完这四块,内核就有了「让任务主动睡下去、被别人唤醒」的能力——这是从「抢占式空转」迈向「真正并发协作」的第一锤。但要把期望放正:这一关全部在**内核态、单核、不关中断**的假设下成立。`acquire()` 里没有 `cli`,正确性靠的是「自旋锁绝不跨阻塞持有」加单核抢占时机受限;多核安全、IRQ 重入安全这两道缺口,要如实承认是「留给以后」,不能假装已经解决。还有一句实话先摆在前面:生产者-消费者 demo 在 `PC_BUF_SIZE=4`、各发 5 个的规模下,producer 大概率一路 `free.wait()` 都不阻塞(初始 `free=4`),真触发阻塞语义的是测试用例,不是 demo——别拿 demo 的串口输出去证明「这里发生了阻塞」。

## 前置条件

你得先过 Lab 020。020 给你留下的地基是这一关全部的依赖:

- **020 的 `Scheduler::block(Task*, const char*)` / `Scheduler::unblock(Task*)`**:`block` 把任务标 `Blocked` 并 `schedule()` 切走、`unblock` 把任务标 `Ready` 并塞回就绪队列。这一关的 Mutex/Semaphore 全靠这两个出口实现「睡/醒」,自己不碰调度器内部。
- **020 的 `g_per_cpu.current`**:取「当前任务」的唯一入口。`struct PerCPU { Task* current; uint64_t kernel_stack; };` 加一个全局 `g_per_cpu`(单核静态占位,不是真 per-CPU 区域)。Mutex 记 owner、Semaphore 入队 waiter,都得拿它当「自己是谁」。
- **020 的 `Spinlock` 定义**:内联在 `sync.hpp`,这一关要把它升级成 out-of-line 并真正上内存序。
- **020 的 `Scheduler::init()` / `TaskBuilder` / `run_first` / idle 兜底**:demo 和测试都要靠这套把两个线程起起来、再切到第一个真任务。

还要确认更早两件没掉链子:**017 的内核堆**(`new` / `knew`,TCB 从堆分配,且堆分配会清零,这点这一关特别要紧)、**019 的 `CpuContext` 布局**(`block` 触发的 `context_switch` 要靠它正确换栈)。

外部约定上,这一关和编译器/硬件契约最紧的两条:一是 **GCC `__atomic` builtins**——`__ATOMIC_ACQUIRE` / `__ATOMIC_RELEASE` 的 happens-before 语义、`__atomic_test_and_set` / `__atomic_clear` 适用于 1/2/4/8 字节标量,是 Spinlock 内存序的全部依据;二是 **POSIX `sem_post(3)`** 的语义(post 先自增、自增后若原本有等待者就唤醒一个)——Cinux 的 `Semaphore::post()` 在设计上对齐它,但 POSIX 那套 `SEM_VALUE_MAX` 上限、async-signal-safe 性质 Cinux **没有实现**,只作对比,别说成已有。

## 任务分解

**第一步:`Task` 加一个侵入式链表指针。** 给 `struct Task` 加一个 `Task* wait_next;`。这一个字段同时服务 Mutex 和 Semaphore 的等待队列——单链表,免堆分配(不用为等待队列再 `new` 节点)。想清楚为什么用侵入式而不是 `knew` 一个队列节点:Mutex/Semaphore 的等待队列是高频短生命周期的结构,每次阻塞都 `new`/唤醒都 `delete` 节点,既慢又容易在堆碎片上翻车;直接复用任务自己身上的一个 `wait_next` 指针,入队出队全是指针搬运,零分配。这里有个**必须诚实承认的细节**:`TaskBuilder::build()` 并不会显式把 `wait_next` 置零——它依赖底层堆分配(`knew`)对新对象的清零。所以「`build()` 出来的任务 `wait_next == nullptr`」这个性质,靠的是堆分配的零初始化,不是 `build()` 的显式赋值。如果你哪天换了个不清零的分配器,这一条会立刻坏掉。

**第二步:`Spinlock` 搬出头文件,上内存序。** 020 的 `Spinlock` 是个内联占位,这一关把它挪进 `sync.cpp`。`acquire()` 的写法是一个 `while (__atomic_test_and_set(&locked_, __ATOMIC_ACQUIRE))`,失败时循环体里插一条 `__asm__ volatile("pause");`,成功才退出;`release()` 一行 `__atomic_clear(&locked_, __ATOMIC_RELEASE)`。`locked_` 的类型是 `volatile bool`,初值 `false`。想清楚这两个内存序为什么这么配:`__ATOMIC_ACQUIRE` 锁住临界区的**入口**——它保证 acquire 之后的所有读写在它之前不会被重排,换句话说,拿到锁的人看得到持锁者上次 release 之前的全部写操作;`__ATOMIC_RELEASE` 锁住临界区的**出口**——保证 release 之前的所有写操作在 release 之前对其它 CPU 可见。`pause` 不是内存屏障,它只是给 CPU 一个提示:「我在自旋等一个会被别人改的变量,别把流水线塞满、别浪费功耗」,在超线程上还能让出执行资源。**整段 `acquire`/`release` 这一关不贴**,但这两条内存序和一个 `pause` 是不可少的。还要钉死一条铁律:**自旋锁只用来保护「几行元数据」(改 `owner_`、改 `count_`、动等待队列),绝不跨阻塞持有**。原因下一节展开。

**第三步:免堆分配的侵入式等待队列。** Mutex 和 Semaphore 各自要一对私有的 `enqueue_waiter(Task*)` / `dequeue_waiter()`,操作的是 `Task* wait_head_` 单链表。两条不变量钉死:(一)FIFO——`enqueue` 尾插、`dequeue` 头摘,保证先阻塞的先被唤醒,这是后面 `unlock` 交接、`post` 唤醒能讲「公平」的根基;(二)擦链——`enqueue` 入口必须先把入队节点的 `wait_next` 置零、`dequeue` 出口必须把出队节点的 `wait_next` 也置零,两条都因为 `wait_next` 复用的是任务身上的字段,可能带着上一次排进别的队列时留下的旧指针,接进新队列就是野指针。具体空链分支怎么写、指针怎么搬,这一关不替你展开——想清楚「尾插保 FIFO、入队出队各擦一次链」这两条,再把空链这个边界单独处理就够了。

**第四步:`Mutex` 的 `lock` / `unlock`(这一关的核心)。** 骨架层面:`lock()` 进临界区判 `owner_`——空闲就拿(记下当前任务当 owner)走人,被占就把自己挂进等待队列再睡;`unlock()` 取队首——没人等就清空 owner,有人等就把所有权交给它再唤醒。两条铁律是这一步的全部难点,具体的 `if` 怎么排、`owner_` 何时赋值,得你自己落到语句里:

- **先释放自旋锁,再 `block`**。`lock()` 里竞争分支把自己入队之后,`spin_.release()` 必须排在 `Scheduler::block(self, ...)` 之前。想清楚反过来会怎样:`block` 内部会 `schedule()` 切到别的任务,而此时自旋锁还握在你手里;下一个任务若也要这把锁(很可能,你们在抢同一份共享数据),它的 `lock()` 会在 `spin_.acquire()` 上死等,而能释放这把锁的你已经被切走、再不运行——经典死锁。所以「先 release 再 block」不是风格偏好,是死锁规避的硬要求。
- **`unlock` 取到队首 waiter 时,把 owner 交接给它,而不是清空**。为什么不走「清空 + 重新抢」:单核抢占下,`unlock` 刚 release 自旋锁、紧接着把 waiter 标 Ready,但此刻 CPU 还在 unlock 手里;如果改成清空 owner,下一个被调度的任务有可能不是队首 waiter(idle 或别的就绪任务先插队),它 `lock()` 就会把锁抢走,队首 waiter 白等了——丢唤醒。直接交接 owner,保证「最先等的那个最先拿到锁」,所有权从不悬空。

`try_lock()` 是 `lock()` 的「不睡」版本:进临界区判 `owner_`,非空就 release 返回 false,否则记 owner、release 返回 true,绝不 `block`。`guard()` 返回一个 RAII `Guard`,构造 `lock`、析构 `unlock`,标 `[[nodiscard]]` 防止漏接。完整的语句级实现这一关不贴,但「先 release 再 block」「unlock 交接 owner 而非清空」这两条,你得自己写进去并想通为什么。

**第五步:`Semaphore` post/wait/try_wait。** `Semaphore` 的状态是 `int64_t count_`(允许为负)加一个 `Task* wait_head_`,构造函数 `explicit Semaphore(int64_t initial = 0)`。骨架层面:`post()`(V 操作)动 `count_` 再决定唤醒谁、`wait()`(P 操作)动 `count_` 再决定睡不睡,两者都遵守「自旋锁只护 `count_`/`wait_head_`,`unblock`/`block` 放到锁外」这条和 Mutex 同源的纪律。两条不变量是这一步的命门,具体的 `if` 分支怎么排,得你自己落语句:

- **`wait` 先 `count_--` 再判 `>= 0`**。不管够不够,先进锁就扣一;减完仍 `>= 0` 说明本来有富余,拿走走人;减成负数,说明资源不够、自己得睡。而**负数的绝对值就是当前在队列里阻塞的等待者数**(`count_ == -3` 意味着有 3 个任务在等)——这个不变量是 `post` 唤醒逻辑的依据。别写成「先判 `count_ > 0` 再减」,那样 `count_ == 0` 时的等待者计数会和实际阻塞数对不上,「负 count 绝对值 = 等待者数」就塌了。
- **`post` 先 `count_++` 再取队首,释放自旋锁之后再 `unblock`**。先自增、再 dequeue:如果之前 `count_` 是负数,加完仍可能是负或零,这时队列里一定有等待者,dequeue 就能取到一个;队列空时资源存进 `count_` 等下次 `wait` 来取。顺序反了(先 dequeue 再 `++`),在「count 正好从 0 走到 1 但队列里其实还有因之前 `count<0` 而睡着的 waiter」这种边界上会错乱,对齐不上 POSIX `sem_post`。

`try_wait()` 不阻塞:进临界区,`count_ <= 0`(0 没货、负数有人在等)就 release 返回 false,否则 `count_--`、release 返回 true——只在 `count_ > 0` 时扣减。`count()` 只读返回 `count_`,诊断用。完整的语句级实现这一关不贴,但「先 `count_--` 再判阻塞」「`post` 先 `count_++` 再唤醒、unblock 在锁外」这几条顺序是死的,反了语义就对不上。

**第六步:`main.cpp` 的生产者-消费者 demo。** 020 那六个空转忙循环换掉。定义一个 `PC_BUF_SIZE = 4` 的共享环形缓冲 `int g_pc_buf[PC_BUF_SIZE]`,三件全局原语:`Semaphore g_sem_free(PC_BUF_SIZE)`(空位数,初值 4)、`Semaphore g_sem_used(0)`(已填充数,初值 0)、`Mutex g_pc_mutex`(保护缓冲本体)。`producer` 循环 5 次:先 `g_sem_free.wait()` 占一个空位 → 进 `g_pc_mutex.guard()` 的 RAII 作用域写缓冲 → 出作用域解锁 → `g_sem_used.post()` 宣告「多了一个可消费的」→ `kprintf("sent: %d\n", i)`。`consumer` 对称:先 `g_sem_used.wait()` 等一个可消费的 → 进 `guard()` 作用域读缓冲 → 出作用域 → `g_sem_free.post()` 宣告「空出了一个位」→ `kprintf("got: %d\n", val)`。这里的对称结构要想通:`free.wait` / `used.post` 是 producer 的两侧,`used.wait` / `free.post` 是 consumer 的两侧,缓冲本体始终在 `Mutex` 保护下被读写。`Mutex` 的 `guard()` 为什么好:写临界区时不用担心「中间 return 忘了 unlock」,作用域结束自动解锁——这一关的 demo 是 RAII 最干净的示范。**顺序仍是铁律:先 `Scheduler::init()`,先 `build` 出 producer/consumer 两个任务并 `add_task`,再 `PIC::unmask(0)`/`(1)`,最后 `sti`,最后 `run_first(&boot_task)`。** 顺序反了,时钟中断抢在原语/任务就位前触发,行为不可预期。串口预期看到 `sent: 0..4` / `got: 0..4`(顺序可能交错,但两端各五个数都得出现,且 `got` 出现的值集合 = `sent` 的值集合)。前面已经说过实话:这个规模下 producer 大概率不阻塞,所以 demo 验的是「三件套拼起来不死锁、不丢数」,不是「这里真发生了阻塞」。

## 接口约束

你要实现/改动出来的东西,对外长这样(职责与签名,不给实现):

- `struct Task` 新增 `Task* wait_next;`(侵入式等待队列链表指针,服务 Mutex 和 Semaphore)。
- `class Spinlock`:`void acquire();`(`__atomic_test_and_set` ACQUIRE + `pause` 自旋)/ `void release();`(`__atomic_clear` RELEASE)/ `[[nodiscard]] auto guard();`(RAII);私有 `volatile bool locked_ = false;`。
- `class Mutex`:`void lock();` / `void unlock();` / `bool try_lock();` / `[[nodiscard]] auto guard();`;私有 `Spinlock spin_;`、`Task* owner_ = nullptr;`、`Task* wait_head_ = nullptr;`、`void enqueue_waiter(Task*);` / `Task* dequeue_waiter();`。
- `class Semaphore`:`explicit Semaphore(int64_t initial = 0);` / `void post();` / `void wait();` / `bool try_wait();` / `int64_t count() const;`;私有 `Spinlock spin_;`、`int64_t count_;`、`Task* wait_head_ = nullptr;`、同上两个私有队列辅助。
- `main.cpp`:`PC_BUF_SIZE=4`、`g_sem_free(PC_BUF_SIZE)`、`g_sem_used(0)`、`g_pc_mutex`,producer/consumer 两个 `TaskBuilder` 任务。

关键约束(违反就翻车):

- **自旋锁绝不跨阻塞持有**。`Spinlock` 在 Mutex/Semaphore 里只盖住「改 owner/count、动等待队列」那几行,`block` 之前必须已 `release`。反例:`Mutex::lock` 先 `enqueue` 再 `block` 却忘了中间 release,`block` 触发 `schedule` 切走,自旋锁烂在手里,下一个抢锁者死等。
- **释放自旋锁必须在 `block` 之前**。`Mutex::lock` 和 `Semaphore::wait` 里,`spin_.release()` 必须排在 `Scheduler::block(self, ...)` 之前。这是上一条的落地形式,单列出来是因为它最常被写反。
- **`unlock` 交接 owner,而非清空**。`Mutex::unlock` 取到队首 waiter 时,把 `owner_` 设成这个 waiter,而不是 `nullptr`;`unblock` 这个 waiter,它醒来后从 `block` 返回、`lock` 直接拿到(因为它已经是 owner 了)。清空 owner 会让别的任务插队抢走锁,丢唤醒。
- **`Semaphore::wait` 先 `count_--` 再判 `>= 0`**。不要写成「先判 `count_ > 0` 再 `--`」——那样 `count_ == 0` 时的等待者计数和实际阻塞数会对不上,「负 count 绝对值 = 等待者数」这个不变量就塌了。
- **`Semaphore::post` 先 `count_++` 再取队首**。顺序反了(先 dequeue 再 `++`),在「count 正好从 0 走到 1 但队列里其实还有因之前 `count<0` 而睡着的 waiter」这种边界上会错乱。对齐 POSIX `sem_post`:先自增,再决定要不要唤醒。
- **`enqueue` 必须先清入队节点的 `wait_next`、`dequeue` 必须清出队节点的 `wait_next`**。不清,链表残留就会变成野指针。
- **`wait_next` 靠堆零初始化,`build()` 不显式置零**。你可以(也应该)在测试里断言 `build()` 出来的任务 `wait_next == nullptr`,但要知道这条性质依赖 `knew` 的清零,不是 `build()` 的功劳。
- **demo 先建任务再 `sti`**。和 020 同理:`init` + 两个 `build` + `add_task` 全部完成后,才 `PIC::unmask` + `sti` + `run_first`。

`acquire`/`release` 里具体用哪个 `__atomic_*`、`pause` 写成内联汇还是别的形式、`Mutex` 五步里每一步的 `if` 怎么排、环形缓冲的下标用 `i % PC_BUF_SIZE` 还是 head/tail——这些这一关不替你定死,但你定下来就要和「先 release 再 block」「unlock 交接 owner」「负 count = 等待者数」这三条不变量对齐。

## 验证步骤

这一关的测试分两层,缺一不可。

第一层是 **host 单元测试**,`test/unit/test_sync.cpp` 在 host 侧重写一份 Spinlock/Mutex/Semaphore 的纯逻辑(不链内核、不跑汇编、用 `std::atomic` 和 mock scheduler/per_cpu),覆盖:Spinlock 初态/acquire-release/RAII guard/double release 良性;等待队列 enqueue/dequeue/FIFO/空队/`wait_next` 清零;Mutex lock 设 owner、unlock 清 owner、try_lock 成功失败、竞争 block+入队、所有权交接、FIFO 三等待者、RAII guard;Semaphore 初值/默认 0/post++/wait 正数不阻塞/wait 到负阻塞/try_wait 成功失败/边界(大初值、repeated post)/lock-unlock 复用;`Task::wait_next` 零初始化为 null:

```bash
ctest --test-dir build -R sync --output-on-failure
```

第二层是 **QEMU 机内集成测试**,`kernel/test/test_sync.cpp` 在真 Scheduler/PMM/VMM/Heap 环境里跑(测试入口 `run_sync_tests()`,机内节名 `Sync Tests (021)`),验的是真原子操作 + 真 `Scheduler::block`/`unblock`:

```bash
cmake --build build --target run-kernel-test
```

机内会打节名 `=== Sync Tests (021) ===`,共 21 个用例,覆盖 Spinlock acquire+guard、Mutex lock/try_lock(free/held)、Mutex 竞争 block+入队 / unlock 交接 / FIFO 三等待者、Mutex RAII、Semaphore 初值/默认 0/post++/wait 正数/wait 到 0 阻塞(count→-1)、try_wait 成功/0 失败/耗尽、post 唤醒 / FIFO、生产者-消费者计数模型(无阻塞路径)、`Task::wait_next` build 后为 null。末尾应有 `[TEST] ALL TESTS PASSED (exit code 0)`(big kernel 的收尾行带 `[TEST]` 前缀、没有 `===` 框;注意它和节名 `=== ... ===`、以及 mini kernel 的 `=== Loaded ELF ... ===` 不是同一种格式,grep 时别认混)。一个明确的失败信号:如果 mini kernel 测试通过了、却停在 `=== Loaded ELF is not a real kernel, exiting ===`、big kernel 测试根本没跑起来——你撞上了这一关的魔数检查坑(见下一节最后一条)。

最后跑**生产 demo** 本身:

```bash
cmake --build build --target run
```

串口应看到 producer 打出 `sent: 0` 到 `sent: 4`、consumer 打出 `got: 0` 到 `got: 4`,两端各五个数,`got` 的值集合与 `sent` 一致。注意 demo 验的是「三件套拼起来不死锁、数据不丢不重」,不是「这里真发生了阻塞」——阻塞语义由上面两层测试覆盖。

## 常见故障

- **`Mutex::lock` 里自旋锁握到 `block` 之后才释放,系统死锁**:第 (4) 步 release 和第 (5) 步 block 写反了。根因是 `Scheduler::block` 内部会 `schedule()` 切走,而自旋锁还握在当前任务手里,下一个抢同一把锁的任务在 `spin_.acquire()` 上死等,而能解锁的你已经被切走。修复:`enqueue_waiter` 之后、`block` 之前,先 `spin_.release()`。防复发:凡是「自旋锁保护 + 后续要阻塞」的组合,把释放点钉死在阻塞调用前一行。

- **`Mutex::unlock` 把 `owner_` 清成 `nullptr` 而非交接给 waiter,队首 waiter 永远等不到锁 / 被后来者插队**:写成了「释放即清空」的直觉写法。根因是单核抢占下,`unlock` 之后下一个被调度的未必是队首 waiter(可能 idle 或别的就绪任务先跑),它 `lock()` 抢到空锁,队首 waiter 白等。修复:`unlock` 取到队首 waiter 时,把 `owner_` 设成这个 waiter 再 `unblock`,所有权直接交接。防复发:阻塞锁的唤醒要走「交接」语义,不要走「清空 + 重抢」,否则 FIFO 和「不丢唤醒」都守不住。

- **`Semaphore::wait` 写成「先判 `count_ > 0` 再 `--`」,阻塞者计数对不上、唤醒错位**:把 `count_--` 放到了判空之后。根因是「负 count 绝对值 = 等待者数」这个不变量依赖「先减再判」:`count_ == 0` 时来一个 `wait`,正确做法是 `--` 成 `-1`、入队阻塞,等待者数=1;若写成先判后减,`count_ == 0` 直接判「不够」就入队,但 `count_` 没动,后续 `post` 唤醒逻辑会和真实等待者数错位。修复:`wait` 第一件事就是 `count_--`,再判 `>= 0`。防复发:`Semaphore` 的 P 操作必须是「扣减先行」,V 操作必须是「自增先行」,顺序和 POSIX `sem_wait`/`sem_post` 对齐。

- **等待队列里出现野指针,`dequeue` 取出个烂地址就崩**:`enqueue_waiter` 没把入队节点的 `wait_next` 先置零,或 `dequeue_waiter` 没把出队节点的 `wait_next` 清掉。根因是 `wait_next` 复用的是任务身上的字段,它可能带着上一次入队留下的旧指针(比如这个任务刚从 Mutex 队列出来、又进了 Semaphore 队列)。修复:`enqueue` 入口 `task->wait_next = nullptr;`,`dequeue` 出口把取出的 `task->wait_next = nullptr;`。防复发:侵入式链表的入队/出队必须各自把节点的那根链指针擦干净。

- **`build()` 出来的任务 `wait_next` 不是 `nullptr`,等待队列一接就乱**:这一条不是你写错了,是底层假设塌了。根因是 `TaskBuilder::build()` 并不显式置零 `wait_next`,它依赖 `knew`/`new` 对堆对象的清零;如果你换了分配器或绕过了清零,这一条立刻坏。修复方向:要么保证堆分配清零(这一关的现状),要么在 `build()` 里显式 `task->wait_next = nullptr;`。防复发:侵入式链表节点依赖零初始化时,要么显式置零、要么在测试里加一条「`build()` 后 `wait_next == nullptr`」的断言把它钉死(这一关的 Test 9 / host 测试就是这么做的)。

- **以为 `Spinlock` 在这一关已经「关中断」或多核安全了**:认知错。`acquire()` 里没有 `cli`,也没有任何多核同步手段。根因(认知上)是 021 是单核(`g_per_cpu` 是一个静态全局 `PerCPU`),正确性靠的是「自旋锁绝不跨阻塞持有」加单核抢占时机受限;多核或 IRQ 嵌套下这把锁并不安全。修复:正文和注释里如实说「单核、不关中断,多核/IRQ 安全缺口留给以后」,不要把 `__ATOMIC_ACQUIRE` 当成「关中断」的替代品——它只管内存可见性,不管「中断在临界区中间发生」。防复发:区分「内存序」(原子操作给的事)和「中断屏蔽」(要 `cli`/`sti` 才有)是两回事。

- **mini kernel 测试全过、却停在 `Loaded ELF is not a real kernel, exiting`,big kernel 测试进不去**:加完 `sync.cpp`(或任何让 BSS 长大的改动)之后,mini kernel 的入口魔数检查把真内核误判成「不是真内核」。根因是 `_start` 头两条是 `cli` + `mov rsp, $__kernel_stack_top`,GNU assembler 对后者有两种合法编码——`48 BC`(imm64)或 `48 C7 C4`(imm32,当立即数可符号扩展到 64 位时 assembler 选更短的);BSS 一长大,`__kernel_stack_top` 的低 32 位恰好可符号扩展,assembler 就从 `48 BC` 换成 `48 C7`,而旧魔数检查只认 `48 BC`。修复:把 `kernel/mini/test/main_test.cpp` 的检查放宽成 `(code[2] == 0xC7 || code[2] == 0xBC)`。防复发:任何基于机器码字节的魔数检查都要枚举所有等价编码——x86-64 的 `mov` 一条指令就能编出两种字节,assembler 会按立即数范围挑最短的,而链接地址(BSS 大小)的变化就能改它的选择。

## 通过标准

1. `Spinlock` 从头文件搬进 `sync.cpp`,`acquire()` 用 `__atomic_test_and_set(&locked_, __ATOMIC_ACQUIRE)` 自旋并插 `pause`,`release()` 用 `__atomic_clear(&locked_, __ATOMIC_RELEASE)`;内存序 acquire/release 分别锁住临界区入口/出口。
2. `Task` 新增侵入式 `Task* wait_next`,Mutex/Semaphore 各有私有 `enqueue_waiter`(尾插)/`dequeue_waiter`(头摘)构成 FIFO 等待队列,且入队/出队都擦干净节点的 `wait_next`。
3. `Mutex::lock()` 五步(取自旋锁 → 空闲则记 owner 返回 → 竞争则入队 → **先 release 自旋锁再 `block`**);`unlock()` 取到队首时**把 owner 交接给 waiter 而非清空**,再 `unblock`;`try_lock()` 不阻塞;`guard()` RAII 且 `[[nodiscard]]`。
4. `Semaphore` 允许 `count_` 为负;`wait()` **先 `count_--` 再判 `>= 0`**,负则入队 → 释放自旋锁 → `block`;`post()` **先 `count_++` 再取队首**,释放自旋锁后 `unblock`;`try_wait()` 只在 `count_ > 0` 时扣减;「负 count 绝对值 = 等待者数」不变量成立。
5. host 单测 `ctest -R sync` 全绿(Spinlock/Mutex/Semaphore/等待队列/`wait_next` 零初始化);QEMU 机内 `Sync Tests (021)` 21 个用例全过、末尾 `[TEST] ALL TESTS PASSED`。
6. 生产 demo:`main.cpp` 用 `g_sem_free(4)` / `g_sem_used(0)` / `g_pc_mutex` 跑通 producer/consumer,串口出现 `sent: 0..4` / `got: 0..4`,两端值集合一致,无死锁。
7. mini kernel 入口魔数检查同时接受 `48 C7`/`48 BC` 两种编码,加 `sync.cpp` 后 BSS 变动不会让 big kernel 测试被误判为「不是真内核」。

七条都达成,内核就第一次有了「让任务主动睡下去、被别人精确唤醒」的同步原语,并把 020 那对没人用的 `block`/`unblock` 接成了真东西。但下一站 022 的钩子也在这里:这一关的原语全在**内核态、单核、不关中断**下成立,`Task::addr_space` 在 demo 里根本没填。022 要进 ring3——用户态进程、syscall、SFMASK/MSR、中断门改 IF——那一章会重新审视「自旋锁在用户态和中断里到底安不安全」,把这一关留下的多核/IRQ 缺口往真正的可抢占方向推。再往后才是 `PerCPU` 从「单核全局」长成「真 per-CPU」、以及更复杂的同步原语(读写锁、条件变量、优先级继承)——那些这一关都没有,别提前当成已有。
