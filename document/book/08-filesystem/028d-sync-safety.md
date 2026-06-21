---
title: 028d · 并发安全:给共享数据上锁
---

# 028d · 并发安全:给共享数据上锁

> 028c 的结尾埋了一句伏笔:我们这一路的写操作「直接写盘」、操作之间「没有任何同步保护」。这一章来兑现这句伏笔——但只兑现一半,而且得先把话说清楚。028d 给内核里所有共享的可变状态(PMM、堆、调度运行队列、fd 表、文件偏移……)上锁或者改成原子,让它们在「多个内核线程 + 时钟中断」的并发轰炸下不再互相踩。这是**并发一致性**。至于你可能更想要的那种一致性——掉电不丢数据、写到一半断电也能回滚(journal、事务)——028d **不碰**,那是更后面的事。先把「数据竞争必然崩」这一刀挡住。换句话说,这一章不是新功能,是一次全内核的安全加固。
>
> 读完你会理解:Cinux 的自旋锁为什么有两种 RAII、调度器为什么必须关着中断加锁、为什么「持着自旋锁去阻塞」是不可饶恕的死罪,以及我们怎么用三层测试证明这套锁在真·抢占环境下站得住。

## 028c 留下的雷,到底有多响

先回顾一个容易被忘掉的事实:调度器从 020 起就是**抢占式**的——RoundRobin,每个任务跑 `DEFAULT_TIME_SLICE = 2` 个 tick 就被换下;PIT 从 011 起就在以 100 Hz 发 IRQ0。也就是说,内核线程随时会被时钟中断打断、切到另一个线程去跑。

而 PMM、堆、调度运行队列、fd 表这些全局可变状态,在 028c 之前是**完全裸奔**的。单线程时代这没事,但抢占式调度一来,就是定时炸弹。举三个真实的雷:

**雷一:PMM 的 find-then-set。** `alloc_page` 的核心是「找一个空闲 bit、把它置位、返回对应的物理地址」。如果两个线程几乎同时进来,都跑到 `bm_find_first_free`,**都看到 bit N 是 0**(空闲),于是各自置位、各自返回 `N * 4096`。结果:**同一个物理页被分给了两个线程**。后面谁先写谁先占,另一个的内容就被无声覆盖。这种 bug 不会立刻崩,而是「偶发、跑久了才炸、换个编译选项又消失」——最难调的那一种。

**雷二:堆的 `free_list` 指针竞争。** 堆的空闲块是条单链表。两个线程同时 alloc/free,一个正在改 `free_list_` 指针,另一个读到的就是改到一半的指针——要么野指针触发 page fault,要么链表被接成环,后面所有分配都在环里打转。

**雷三:调度运行队列被两种上下文碰。** 它不只被线程碰(`yield`/`add_task`/`block` 会调 `enqueue`/`dequeue`),还被**中断上下文**碰:时钟中断 → `tick()` → 时间片到了 → `schedule()` → `pick_next()`,全在改同一条运行队列。要是 `pick_next` 在你改队列改到一半时插进来……

DONE.md 把它们归成一句:「TIER 0 核心分配器——数据竞争必然崩溃」。不是危言耸听,是这一类共享状态在并发下**一定**会出问题,只是早晚。

## 先造基建:四个同步原语

在动手加固之前,得先有趁手的工具。028d 新增的 `kernel/proc/sync.{hpp,cpp}` 一口气造了四个原语,分两层:

```text
        ┌──────────────────────────────────────────────────────┐
        │  上层:阻塞原语(复用调度器 block/unblock,会真睡)        │
        │      Mutex                   Semaphore(P/V)          │
        │  内部各持一把 Spinlock,只用来护自己的 owner / wait queue │
        └────────────────────────┬─────────────────────────────┘
                                 │ 依赖
        ┌────────────────────────▼─────────────────────────────┐
        │  地基:忙等原语(不睡,死循环抢)                          │
        │      Spinlock  ── test-and-set + pause                │
        │         ├─ guard()      不关中断,只挡别的线程           │
        │         └─ irq_guard()  关中断 + 自旋,挡线程也挡中断     │
        │      InterruptGuard ── 纯关中断(pushfq/cli/popfq)      │
        └──────────────────────────────────────────────────────┘
```

从下往上看。

### Spinlock:忙等地基

```cpp
class Spinlock {
    volatile bool locked_ = false;

    void acquire() {
        while (__atomic_test_and_set(&locked_, __ATOMIC_ACQUIRE)) {
            __asm__ volatile("pause");
        }
    }
    void release() {
        __atomic_clear(&locked_, __ATOMIC_RELEASE);
    }
};
```

`__atomic_test_and_set` 干一件事:原子地「把 `locked_` 置成 1,返回它置之前的旧值」。旧值是 1(别人持着),就继续 `while` 转;旧值是 0(被我抢到了),就跳出循环往下走。

两个细节值得停下来看。`__ATOMIC_ACQUIRE` / `__ATOMIC_RELEASE` 这对内存序,保证的是「acquire 之后的读,看得到 release 之前的写」——临界区里的内存访问不会被 CPU 或编译器重排到锁的外面去,否则你「加了锁」也挡不住别人看到半成品数据。`pause` 是 x86 给 CPU 的一个提示:「我在自旋等一个变量」。没有它,CPU 会一路猛推测执行,等到锁终于被释放、自己抢到时,会触发一次 memory-order 违规的**重罚**(流水线冲刷);`pause` 还顺带大幅降低等待时的功耗。这是 Intel SDM 对 `PAUSE` 指令用途的明确说明,不是我们瞎加的。

关键在它提供**两种** RAII 守卫,这是整章设计的命门:

```cpp
// 不碰中断,只挡「别的内核线程」
[[nodiscard]] auto guard()     { return Guard(this); }      // acquire / release

// 关中断 + 自旋,挡线程也挡中断
[[nodiscard]] auto irq_guard() { return IrqGuard(this); }   // pushfq,cli,acquire / release,restore
```

为什么必须有两种?留到「设计现场 A」展开。先记住一句话:**这块数据如果会被中断处理路径碰到,就得用 `irq_guard()`,否则会死锁;只在线程之间共享的,用普通 `guard()` 就够。**

### InterruptGuard:纯关中断

```cpp
InterruptGuard::InterruptGuard() {
    __asm__ volatile("pushfq; popq %0; cli" : "=rm"(saved_flags_));   // 存 RFLAGS,关中断
}
InterruptGuard::~InterruptGuard() {
    __asm__ volatile("pushq %0; popfq" : : "rm"(saved_flags_));        // 恢复 RFLAGS
}
```

它不拿锁,只把 RFLAGS 里的 IF 位(中断允许标志,第 9 位,掩码 `0x200`)清掉,顺便记住原来的值。用在「跟某个中断处理程序共享、但又不需要在多线程之间互斥」的场景——典型就是键盘的环形缓冲。

它对**嵌套**是安全的:内层 guard 存到的 `saved_flags_` 本身就是「IF=0」,所以内层析构时恢复的还是 IF=0,只有最外层析构才把 IF 恢复成进入前的样子。kernel 测试里的 `test_nested_guard` 专门断言了「内层进去出来,IF 一直是 0;最外层出来,IF 回到原值」。

### Mutex / Semaphore:造好了,但这一章还没让它们上岗

`sync.hpp` 还定义了 `Mutex` 和 `Semaphore`:

```cpp
class Mutex {
    Spinlock spin_;            // 只护下面这三个字段,护的时间极短
    Task*   owner_     = nullptr;
    Task*   wait_head_ = nullptr;   // 侵入式等待队列,借 Task::wait_next 串起来
};
```

它们是**阻塞式**原语:抢不到锁不忙等,而是把自己挂到等待队列、调 `Scheduler::block()` 睡过去,由 `unlock()` / `post()` 的一方唤醒。`Semaphore` 是经典 Dijkstra 信号量(`wait`=P、`post`=V)。

**但这里必须诚实一句**:在 028d,`Mutex` 和 `Semaphore` **只被定义和测试,还没有任何生产代码用它们**。你 grep 整个 `kernel/`(排除 `sync.cpp`/`sync.hpp` 自身和测试目录)会发现一个 `.lock()` 调用都没有——这一章真正上线防护的,全是 `Spinlock`(`guard()` 或 `irq_guard()`)、`InterruptGuard` 和原子计数器。`Mutex`/`Semaphore` 是为后面那些需要「真 sleep」的场景(等磁盘、等信号、等条件)预备的弹药。我们仍然要讲它们的实现,因为里面有一条「release-before-block」的纪律太经典、现在就得讲明白——见设计现场 B。记住这个边界,免得你以为 028d 已经用上了阻塞锁。

## 三层加固落地

造完原语,就是大面积套用。DONE.md 把这次加固按风险分了三层,我们也按这个来看。

### 谁加了什么、为什么是这种原语

| 子系统 | 保护方式 | 为什么选它 |
|---|---|---|
| PMM `alloc/free` | Spinlock 普通 `guard()` | 只有内核线程碰,中断不碰 |
| Heap `alloc/free` | Spinlock 普通 `guard()` | 同上 |
| **Scheduler 运行队列** | Spinlock **`irq_guard()`** | 被 PIT IRQ0 的 `tick()` 路径碰,**必须关中断** |
| FDTable `alloc/close/get` | Spinlock 普通 `guard()` | 线程间;防 double-close / use-after-free |
| `PIT::tick_count_` | `std::atomic` | 单字段、超高频,加锁太贵 |
| Scheduler `tick_count_`/`current_slice_`、`next_tid`、`next_stack_vaddr` | `std::atomic` | 同上,热路径上的单字段 |
| `File::offset` | Spinlock 普通 `guard()`(`offset_lock_`) | sys_read/write/getdents 改偏移 |
| VMM `map/unmap` | Spinlock 普通 `guard()` | 线程间改页表 |
| `g_mount_table` | static Spinlock 普通 `guard()` | 线程间改挂载表 |
| Keyboard 环形缓冲 | `InterruptGuard` | 与 IRQ1 ISR 共享,单生产者单消费者 |

整张表的判断标准其实只有一句话:**这块数据,中断处理路径会不会碰它?**

- **会** → 走「关中断」那一侧:`irq_guard()`(既要挡别的线程、又要挡中断)、`InterruptGuard`(只挡那个 ISR、不需要多线程互斥),或者原子(单字段、高频)。
- **不会** → 普通 `guard()` 就够,少一次 `cli`/`popfq` 的开销。

### PMM 怎么改的:把 find 和 set 圈进同一把锁

以 PMM 为例看「plain guard 怎么治 find-then-set 的竞争」:

```cpp
uint64_t PMM::alloc_page() {
    auto g = lock_.guard();        // ← 整个「找 + 置位」被圈成一个临界区
    (void)g;
    return alloc_page_locked();    //   find_first_free + bm_set + free_pages--
}
```

028c 之前,「找空闲 bit」和「置位」是分开的两步、且无锁,于是两个线程能各自 find 到同一个 bit。现在 `guard()` 把它们包成一个不可分割的临界区:第二个线程会在 `acquire` 处 spin,等第一个 `release` 之后再去 find——这时 bit 已经被置上了,它自然会 find 到**下一个**空闲位。竞争就这么根治了。

顺带一个小拆分:你会看到 `alloc_page_locked` / `free_page_locked` 这种「不拿锁」的内层函数。原因是 `alloc_pages`(分配连续多页)需要**在已经持锁的状态下**复用单页逻辑——它不能去调会再次 `guard()` 的 `alloc_page`,因为我们的 Spinlock 不支持重入(同一个线程第二次 `acquire` 会把自己 spin 死)。所以把「真正干活但不碰锁」的逻辑抽成 `_locked` 版本,由外层加好锁再调。这是个很常见的锁代码组织手法。

### 调用链:一次 sys_read(fd>0) 穿过哪些锁

把这些锁串起来看,一次读文件实际穿了两个临界区:

```text
sys_read(fd)
  └─ g_global_fd_table().get(fd)          ── 持 FDTable::lock_  (guard)
        └─ 拿到 File*
             └─ { auto g = file->offset_lock_.guard();   ── 持 File::offset_lock_ (guard)
                  inode->ops->read(inode, offset, buf, count);
                  file->offset += result;
                }
```

两次都是普通 `guard()`,因为读路径上没有中断碰这些数据。注意 `offset_lock_` 的粒度——它锁的是**偏移量的「读 + 改」**,不是整个 inode。两个进程通过各自的 fd 读同一个文件,各自有独立的 `offset`、各自的 `offset_lock_`,互不阻塞。这正是 `offset_lock_` 挂在 `File`(打开文件描述)上、而不是挂在 `Inode` 上的原因:每个打开实例有自己的读写位置,不该因为一个进程在读,就把整个文件锁死。

## 设计现场

### A. 调度器为什么必须 irq_guard,不能用普通 guard

这是这一章最硬的一个 why。RoundRobin 的三个方法,每一个开头都是这一句:

```cpp
void RoundRobin::enqueue(Task* task) {
    auto g = lock_.irq_guard();   // ← 关中断 + 自旋,不是普通 guard()
    (void)g;
    ...
}
```

`dequeue`、`pick_next` 也一样。为什么非得关中断?因为运行队列被**两种上下文**同时碰:

- 线程上下文:`yield()` / `add_task()` / `block()` 调用 `enqueue` / `dequeue`;
- **中断上下文**:PIT 的 IRQ0 handler → `Scheduler::tick()` → 时间片到 → `schedule()` → `pick_next()`。

设想你贪省事用了普通 `guard()`(不关中断):线程 A 正在改运行队列、改到一半、**锁还在手里**;这时 100 Hz 的时钟中断来了,硬件立刻打断 A,跳进 `tick() → schedule() → pick_next()`,`pick_next` 也要 `irq_guard` → 先 `acquire` 同一把锁 → 锁被 A 持着 → **spin**。可 A 此刻正被中断打断着、压在栈里没机会执行、更没机会 `release`;而中断处理里你又在那儿死等它 → **死锁,系统当场卡死**,而且卡得无声无息(连打印都打不出来,因为打印也走中断)。

`irq_guard()` 在 `acquire` 之前先 `cli` 把中断关掉,就掐断了「持锁期间被中断重入」这个可能:拿锁的整段时间里,不会有任何中断插进来。这就是「与中断共享的数据,必须用关中断侧原语」的活例子。至于 `tick()` 自己——它本来就跑在中断上下文里,进 ISR 时硬件已经把 IF 清了,它一路调到 `pick_next` 再 `irq_guard` 是幂等的、安全的。

### B. release-before-block:持着自旋锁去阻塞,是死罪

`Mutex::lock()` 里这两步的顺序,反了就是死锁:

```cpp
void Mutex::lock() {
    spin_.acquire();
    if (owner_ == nullptr) { owner_ = g_per_cpu.current; spin_.release(); return; }  // 没人占,直接拿
    Task* self = g_per_cpu.current;
    enqueue_waiter(self);                          // 有人占,排队
    spin_.release();              // ← 必须先释放自旋锁
    Scheduler::block(self, "mutex");   // ← 再阻塞自己
}
```

源码注释原话:「Release the spinlock BEFORE blocking (avoids deadlock)」。为什么顺序不能反?`block()` 会把自己移出运行队列、切到别的线程去跑。如果你**手里攥着 `spin_` 就去 block**,那么将来唯一能唤醒你的那个线程(它要调 `Mutex::unlock()` → `unlock` 第一步就是 `spin_.acquire()` 才能动等待队列)就**永远拿不到这把锁**——锁攥在你这个已经睡死的人手里。于是:你永远等不到唤醒,唤醒者永远等不到锁。经典死锁。

所以这条铁律是:**自旋锁,绝不能跨越任何可能引发调度或阻塞的点**。`block()`、`yield()` 之前必须 `release`。`Semaphore::wait()` 同理照办。这条纪律比任何具体的锁实现都重要——它决定了「哪些原语能和调度器组合、怎么组合」。也是为什么 `Mutex`/`Semaphore` 内部那把 `spin_` 只用来护 `owner_`/`wait_head_` 这么几个字段、护的时间极短——长临界区绝不归它管。

### C. [[nodiscard]] 和那个看起来多余的 `(void)g;`

你会在这一章改过的几乎每个函数里看到这么个有点怪的写法:

```cpp
auto g = lock_.guard();
(void)g;
```

`guard()` 被标了 `[[nodiscard]]`。它防的是这种偷懒写法:

```cpp
lock.guard();   // ← 返回的临时 Guard 在这一行末尾立刻析构,等于没加锁
```

guard 作为临时量,语句一结束就析构、立刻 `release`——**等于根本没加锁**,而编译器默认还不报错(直到运行时数据竞争找上门)。`[[nodiscard]]` 让编译器对「构造了返回值却丢弃」发出告警;`auto g = …` 把它绑定到一个局部变量上,生命期就延续到整个作用域结束,析构时才 `release`——这才是 RAII 加锁该有的样子。至于 `(void)g;`,纯粹是为了消掉「变量 g 声明了却没被读取」的 `-Wunused` 警告,同时**保住它的生命期**(不能因为「没用」就让你去删它)。一个小小的 `(void)`,挡掉一整类「自以为加了锁、其实没加」的 bug。

### D. 键盘为什么用 InterruptGuard,不用 Spinlock

`Keyboard::poll()`(线程侧,负责从环形缓冲取键)的开头是 `InterruptGuard guard;`,不是 Spinlock。键盘的环形缓冲(`head_`/`tail_`/`queue_`)是**单生产者单消费者**结构:生产者是 IRQ1 的 ISR(按键中断里 `enqueue`),消费者是 `poll`。ISR 一进来,IF 已经被硬件清成 0(中断处理天然关中断),所以 `enqueue` 这一侧天然安全;消费侧 `poll` 只要临时关一下中断,就不会和 ISR 抢着改 `head`/`tail`。

这种「只有两方、其中一方是 ISR」的场景,关中断比自旋锁更轻更直接——没必要让一个线程为了等 ISR 的事件去空转。注意它和调度器那种「多线程 + 中断都要互斥」的需求不同:调度器要挡住**别的线程**,所以需要 `irq_guard`(关中断 + 自旋);键盘只需要挡住**那一个 ISR**,所以 `InterruptGuard`(纯关中断)足矣。需求不同,选的原语就不同。

## 验证

028d 的验证分三层,从「机制对不对」到「真并发扛不扛得住」,一层比一层狠。少了哪一层都有漏网的可能。

**第一层:host 单测(std::thread 真线程压测)**

```bash
cmake --build build
ctest --test-dir build -R 'sync_concurrent|^sync$|pmm|heap|scheduler' --output-on-failure
```

`sync_concurrent` 这个可执行文件在 `test/CMakeLists.txt` 里链了 `-pthread`,也就是说它在宿主机上**真的起多个 `std::thread`** 去压自旋锁、互斥、分配器,然后看计数和分配结果对不对。pmm / heap 的 host 测试也在 028d 新增了并发用例——比如 pmm 起多个 `std::thread` 同时 `alloc_page`,断言不会把同一页分给两个线程(正是正文讲的 find-then-set 竞争)。要注意:scheduler 的 host 测试仍是**单线程白盒**(只验运行队列的结构和状态机),它的并发正确性不靠这一层,而是靠下面第二、第三层来压。这一层跑得快、能反复回归,是日常开发的第一道闸。

**第二层:kernel 端 RAII 机制测试(QEMU,验正确性)**

```bash
cmake --build build --target run-kernel-test
```

`big_kernel_test` 启动后,`main_test.cpp` 会调到 `run_sync_concurrent_tests()`(注释标着「Sync concurrent tests (028d): InterruptGuard, IrqSpinlockGuard」),输出里会有 `Sync Concurrent Tests (028d)` 这一段。它逐条断言:

- `InterruptGuard`:进 guard 后 IF 确实被清(`flags & 0x200` 为 0),析构后恢复原值;嵌套场景下内层进出 IF 一直是 0、最外层才恢复。
- `Spinlock::IrqGuard`:持锁期间 IF=0,析构后锁释放、中断恢复;不同锁可嵌套;进入时 IF 本来就是 0 的,析构后仍是 0(不误开)。
- Spinlock / IrqGuard 的互斥:三个 Task 协作式地各自自增若干次,总和精确。
- Scheduler 并发:add/remove 一批 Task 后状态正确,block/unblock 状态转换正确。

这一层是 DONE.md 说的「协作式」多任务测试——它手动在几个 Task 之间切 `g_per_cpu.current`、顺序执行,并不制造真正的并行,但它把关中断、加锁、释放的**每一步**都用断言钉死,保证 RAII 的机制行为正确。

**第三层:生产 stress(真·抢占 + 时钟中断,验并发)**

```bash
cmake --build build --target run-stress-test
```

这是最狠的一层。`stress_test.cpp` 在**生产内核**启动时(shell 之前)起 4 个内核线程,开着抢占和 100 Hz 时钟中断,每个线程做:

- 200 次 `alloc_page` / `free_page`;
- 200 次 `alloc(64)` / `free`;
- 1000 次原子计数器自增。

然后 `boot_continuation` 线程 spin 等全部完成,断言**精确**操作数。这些数字来自 `stress_test.cpp` 里的 `kprintf`,是真实输出格式:

```text
[STRESS] ===== 028d Concurrent Stress Test =====
[STRESS] Threads=4  PMM_ops/thread=200  Heap_ops/thread=200
...
[STRESS] PMM alloc/free ops: expected=800 actual=800 PASS
[STRESS] Heap alloc/free ops: expected=800 actual=800 PASS
[STRESS] Atomic counter:       expected=4000 actual=4000 PASS
[STRESS] ALL PASSED -- launching shell
```

`expected=actual` 看着理所当然,但它的潜台词是:在真抢占 + 真时钟中断的环境下,PMM 没有把同一页分给两个人、堆的 `free_list` 没有被改坏、计数器没有丢更新。如果锁加错了,`actual` 会小于 `expected`(操作在竞争里丢失),或者干脆 triple fault。这层过了,028d 的「并发安全」才算真正站住。

> 三层的分工:host 单测覆盖广、跑得快、天天回归;kernel 机制测试证明「我们的 RAII 在真硬件上行为分毫不差」;生产 stress 证明「这些原语部署之后,在最恶劣的并发形态下也不崩」。机制正确 ≠ 并发安全,必须分开验证。

## 下一站

028d 把内核的共享数据都锁好了,但你会发现一个尴尬的事实:**启动流程本身还是「一根筋」的**。`kernel_main` 从头跑到尾——AHCI 读盘、ext2 mount、起 shell——全是同步的、跑在一个不可调度的上下文里。哪天哪一步需要「等一会」(等磁盘就绪、等一个事件),现在没有干净的地方让它 sleep,因为启动代码压根不在任何可调度的线程上。

028e 要解决的就是这个:把「启动到 shell」这条路径本身,变成一个**可以被调度、可以阻塞、可以让出**的内核线程——init 线程。这么一来,启动逻辑就能复用我们这一章刚造好的 `Mutex`/`Semaphore`、能 `block`、能被时钟中断打断——本章那些「造好但还没上岗」的阻塞原语,终于等到真正的用武之地。

(顺带一提:那个临时的 `stress_test.cpp` 会在 028e 被移除——它是 028d 的阶段性验证脚手架,使命完成后就退场,把启动的主舞台让给 init 线程。)

怎么把启动路径线程化、又怎么避免它和新接进来的驱动撞 MMIO 地址,那是 028e 的故事。

---

**参考**

- Intel SDM Vol 2,`PAUSE` 指令:自旋等待循环的提示,用于避免 memory-order 违规带来的流水线性能惩罚并降低等待功耗——`Spinlock::acquire` 中 `pause` 的依据。<https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html>
- Intel SDM Vol 2,`CLI`/`STI`/`PUSHFQ`/`POPFQ` 与 RFLAGS.IF(第 9 位,掩码 `0x200`):`InterruptGuard`、`Spinlock::IrqGuard` 关中断与恢复的依据;IF 位编号在 kernel 测试 `test_basic_save_restore` 里被直接断言。
- GCC 手册,`__atomic` 内建函数与 `__ATOMIC_ACQUIRE` / `__ATOMIC_RELEASE` 内存序:`__atomic_test_and_set` / `__atomic_clear` 的语义依据。<https://gcc.gnu.org/onlinedocs/gcc/_005f_005fatomic-Builtins.html>
- OSDev Wiki,Spinlock:test-and-set + pause 的社区标准实现路径,以及「关中断自旋锁」与「普通自旋锁」的取舍。<https://wiki.osdev.org/Spinlock>
- Dijkstra 信号量(P/V):`sync.hpp` 中 `Semaphore` 注释直接引用的经典同步模型(`wait`=P、`post`=V)。
