---
title: 02 · 代码路线:Spinlock / Mutex / Semaphore / 生产者-消费者
---

# 代码路线:Spinlock / Mutex / Semaphore / 生产者-消费者

## Spinlock:为什么从内联搬出来,以及内存序

020 的 [sync.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/sync.hpp) 里,`Spinlock` 整个类——包括 `acquire` / `release` 的函数体——都直接写在头文件里。这在「没人用它」时无所谓;可 021 要让 `Mutex` / `Semaphore` 各自持有一把 `Spinlock` 作为内部成员,如果 `Spinlock` 还是 inline 定义,那么每多一个翻译单元 include `sync.hpp`,这套原子操作就被复制一份,符号也满天飞。所以第一步,把 `acquire` / `release` 搬进新建的 [sync.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/sync.cpp),头文件里只留声明:

```cpp
// sync.cpp
void Spinlock::acquire() {
    while (__atomic_test_and_set(&locked_, __ATOMIC_ACQUIRE)) {
        __asm__ volatile("pause");
    }
}

void Spinlock::release() {
    __atomic_clear(&locked_, __ATOMIC_RELEASE);
}
```

实现本身和 020 一字不差,重点是把它「落到实处」。`__atomic_test_and_set(&locked_, __ATOMIC_ACQUIRE)` 是 GCC 的标准原子内建:把目标字节原子地置 1、并返回它的旧值;在 x86 上它编译成带 `LOCK` 前缀的 `xchg` 或等价指令。套在 `while` 里就是「抢到了就出循环,没抢到就一直试」。每次抢失败插一条 `pause`,它是给超线程 CPU 的提示:告诉硬件「我在自旋,别把流水线占满」,顺便规避一段长自旋触发内存序违例惩罚(SDM 把它归为 "Spin-Wait Hint"——概念性说法,手册里那条目我没在本地 PDF 里定位到精确页,故不引页码)。`release` 的 `__atomic_clear` 就是原子地写 0。

两端的内存序要配对:`ACQUIRE` 作用于 `acquire`,保证「拿到锁之后,读到的内存视图」包含此前所有 `RELEASE` 写入的值;`RELEASE` 作用于 `release`,保证「释放锁之前的写」在锁被别人拿走之前对它们可见。这两条把临界区从两头夹住——进去时能看见上一个持锁者的全部修改,出去时保证自己的修改已经落地。少了任何一端,临界区就漏气。

定性再强调一遍:`Spinlock` 在本章只保护几行元数据——`Mutex` 的 `owner_`/`wait_head_`、`Semaphore` 的 `count_`/`wait_head_`,**绝不跨阻塞持有**。任何 `Spinlock` 的持有区间里,都不能出现 `Scheduler::block`、`schedule`、`yield` 这类会切走的调用。为什么?因为切走后下一个任务要是也想拿这把自旋锁,就死等一个「正在睡觉、根本没在跑」的持锁者——死锁。这条纪律是后面 `Mutex::lock()` 那个「先 release 再 block」铁律的源头。

## Task::wait_next:免堆分配的侵入式等待队列

Mutex 和 Semaphore 的等待队列,都不额外分配链表节点,而是直接借 `Task` 身上的一个指针字段。看 [process.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/process.hpp) 里 `Task` 新增的最后一行:

```cpp
struct Task {
    // ... ctx, state, tid, priority, kernel_stack, kernel_stack_top,
    //     addr_space, name, sched_class ...
    Task* wait_next;   // 侵入式链表指针: Mutex / Semaphore 等待队列复用
};
```

复用一个已有结构体的字段做链表,叫侵入式链表(intrusive list)。好处是不用为每个等待者 `new` 一个节点——在内核里尤其值钱,因为 `new` 走的是堆,而持有自旋锁时去分配堆既慢又危险。代价是同一个 `Task` 同时只能排在一个等待队列里——但单核抢占式下,一个任务同一时刻也只会等一样东西,所以这条限制不痛。

队列操作是教科书式的单链表,尾插、头摘:

```cpp
void Mutex::enqueue_waiter(Task* task) {
    task->wait_next = nullptr;          // 先把自己尾巴清干净
    if (wait_head_ == nullptr) {        // 空队: 直接当头
        wait_head_ = task;
        return;
    }
    Task* tail = wait_head_;            // 否则走到队尾, 挂上去
    while (tail->wait_next != nullptr) tail = tail->wait_next;
    tail->wait_next = task;
}

Task* Mutex::dequeue_waiter() {
    if (wait_head_ == nullptr) return nullptr;
    Task* task = wait_head_;            // 摘头
    wait_head_ = task->wait_next;
    task->wait_next = nullptr;          // 摘下来后把自己的 wait_next 也清掉
    return task;
}
```

`enqueue` 第一行 `task->wait_next = nullptr` 不是可有可无:`Task` 复用一个侵入式指针,它上一次被 dequeue 时虽然也清了,但稳妥起见每次入队都重新置空,确保尾巴干净。`Semaphore` 那边是同一份逻辑的拷贝(两个类各自有私有 `enqueue_waiter`/`dequeue_waiter`,没共用——代码重复,但各自闭合,改动互不影响)。

这里有个必须诚实说清的细节:`TaskBuilder::build()` 并**没有**显式把 `wait_next` 置零。机内测试 [test_sync.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_sync.cpp) 的 `test_wait_next_null_after_build` 注释写明了这一点——它依赖的是**底层堆分配会清零**(`new`/`knew` 给出的内存是零初始化的),所以新建出来的 `Task::wait_next` 恰好是 `nullptr`。这是个隐含约定,不是显式保证:哪天换了不清零的分配器,这条就塌了。机内测试专门断言 `task->wait_next == nullptr`,就是为了把这个隐含约定钉住——它一旦不成立,所有等待队列都会拿到野指针乱指。

## Mutex:阻塞式互斥与所有权交接

[sync.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/sync.cpp) 里 `Mutex::lock()` 是五步,前面设计图画过,这里看真身:

```cpp
void Mutex::lock() {
    spin_.acquire();                          // ① 拿内部自旋锁
    if (owner_ == nullptr) {                  // ② 空闲: 自己当 owner, 走人
        owner_ = g_per_cpu.current;
        spin_.release();
        return;
    }
    Task* self = g_per_cpu.current;           // ③ 被占: 取自己
    enqueue_waiter(self);                     //    挂队尾
    spin_.release();                          // ④ 释放自旋锁 (在 block 之前!)
    Scheduler::block(self, "mutex");          // ⑤ 睡过去; 醒来时已被设成 owner
}
```

`g_per_cpu.current` 就是 020 那个单核静态全局里的「当前任务」——Mutex / Semaphore 取「我是谁」全靠它。`Scheduler::block(self, "mutex")` 内部会 `task->state = Blocked`、把它从就绪队列移出,然后因为 `self == current_` 触发 `schedule()` 切走。也就是说,执行到 ⑤ 这一行之后,当前任务就睡过去了,`lock()` 这条调用栈被冻结,直到有人 `unlock` 把它唤醒。

`unlock()` 同样五步,精髓在「直接交接 owner」:

```cpp
void Mutex::unlock() {
    spin_.acquire();                          // ① 拿自旋锁
    Task* waiter = dequeue_waiter();          // ② 摘队首
    if (waiter == nullptr) {                  //    没人等: 清空 owner, 释放, 返回
        owner_ = nullptr;
        spin_.release();
        return;
    }
    owner_ = waiter;                          // ③ 直接把所有权交给 waiter (不清空!)
    spin_.release();                          // ④ 释放自旋锁
    Scheduler::unblock(waiter);               // ⑤ 唤醒新 owner (Ready + 入就绪队列)
}
```

为什么 ③ 把 `owner_` 设成 `waiter` 而不是 `nullptr`?设想另一种写法——清空 owner、再 `unblock` waiter。在这两步之间,如果有第三个任务恰好在别的核(将来)、或别的调度时机来 `lock()`,它会看到 `owner_ == nullptr` 而直接把锁抢走。而那个刚被 `unblock` 的 waiter,醒来时它的 `lock()` 会从 ⑤ 那行继续执行——可它醒来时 `owner_` 已经被第三者改写,它根本没拿到锁,却以为自己排过了队。这就是「丢唤醒」。直接交接:`owner_ = waiter` 一行之后,锁的所有权从未悬空,醒来的人手里就是锁,谁也插不进。`Scheduler::unblock` 把 waiter 标 `Ready` 并 `enqueue` 回就绪队列,等它被 `schedule` 选中、从 ⑤ 那行往下 `return`,它就已经持锁。

`try_lock()` 是 `lock()` 的「不睡」版本:同样在自旋锁里判 `owner_`,空就抢、不空就返回 `false`,绝不 `block`:

```cpp
bool Mutex::try_lock() {
    spin_.acquire();
    if (owner_ != nullptr) { spin_.release(); return false; }
    owner_ = g_per_cpu.current;
    spin_.release();
    return true;
}
```

RAII 这块和 020 的 `Spinlock::Guard` 一个模子:`[[nodiscard]] auto guard() { return Guard(this); }`,构造时 `lock()`、析构时 `unlock()`,标记 `[[nodiscard]]` 是防止写出 `m.guard();` 漏接、临时对象立刻析构等于没加锁。

## Semaphore:计数信号量,负 count 即等待者数

`Semaphore` 的字段比 Mutex 多一个 `count_`,队列逻辑同构。构造函数给初值(默认 0):

```cpp
Semaphore::Semaphore(int64_t initial) : count_(initial), wait_head_(nullptr) {}
```

`wait()` 是本章最值得拆的一段,因为它把「负 count」的不变式用代码固定下来:

```cpp
void Semaphore::wait() {
    spin_.acquire();                          // ①
    count_--;                                 // ② 先扣
    if (count_ >= 0) {                        // ③ 还 >= 0: 资源有剩, 直接走
        spin_.release();
        return;
    }
    Task* self = g_per_cpu.current;           // ④ < 0: 透支了, 自己是等待者之一
    enqueue_waiter(self);
    spin_.release();                          // ⑤ 释放自旋锁 (在 block 之前)
    Scheduler::block(self, "semaphore");      // ⑥ 睡
}
```

注意 ② 的 `count_--` 是**无条件先扣**,然后才在 ③ 判符号。如果写成「先判 `count_ > 0` 再扣」,就得在自旋锁里多一次「判-扣」的来回,而且语义会变:初始 `count_ = 0` 时,「先扣」会让它变 `-1`(正好表「一个等待者」),「先判」会让它停在 `0`(没扣、也没睡,得另写一条阻塞分支)。先扣后判的好处是 `count_` 永远诚实:正数是可用资源数,负数绝对值是等待者数,0 是「刚好没货也没人等」。`count_ >= 0` 就直返(资源够,或刚好 0 但本次没透支),`< 0` 才入队睡——判据极简。

`post()` 对偶:

```cpp
void Semaphore::post() {
    spin_.acquire();                          // ①
    count_++;                                 // ② 先加
    Task* waiter = dequeue_waiter();          // ③ 摘队首(可能为 nullptr)
    spin_.release();                          // ④ 释放自旋锁
    if (waiter != nullptr) {                  // ⑤ 有等待者才唤醒
        Scheduler::unblock(waiter);
    }
}
```

顺序同样是「先动 `count_`、再动队列」,且都在自旋锁里。`count_++` 之后:如果之前有等待者(`count_` 从负数往上加),dequeue 会摘到一个,然后释放锁、唤醒它;如果队列空(dequeue 返回 `nullptr`),那这次 `post` 的资源就存进 `count_` 里,等下次 `wait` 来取。`unblock` 放在自旋锁**外面**——`Scheduler::unblock` 只是把任务标 `Ready` + 入就绪队列,不切走当前任务,但它也可能碰到调度器内部结构,所以原则上别在持自旋锁时调,保持「自旋锁只护 `count_`/`wait_head_`」这条边界干净。

`try_wait()` 只在 `count_ > 0` 时减一返回 `true`,否则 `false`,不睡:

```cpp
bool Semaphore::try_wait() {
    spin_.acquire();
    if (count_ <= 0) { spin_.release(); return false; }   // 注意: <= 0 都不行(0 没货, 负数有人在等)
    count_--;
    spin_.release();
    return true;
}
```

这里判据是 `count_ <= 0` 而不是 `count_ == 0`:因为 `count_` 可以是负数(有人在等),负数时当然也不能 `try_wait` 成功——否则就偷走了一个本该留给等待者的资源。`count_` 还有个只读的 `count()` 给诊断用,host 测试用它断言 `count_` 的演化。

对比一下 POSIX `sem_post(3)`:POSIX 的 `sem_post` 也是「自增,若结果 > 0 则唤醒一个阻塞的 `sem_wait`」,语义和我们的 `post()` 同源。但 POSIX 还附带一堆本章没实现、也不该假装有的性质——`SEM_VALUE_MAX` 上限(我们 `count_` 无界)、`EOVERFLOW` 错误码、async-signal-safe(可在信号 handler 里安全调用,我们的实现**没**这个保证,拿自旋锁进信号 handler 是另一套麻烦)。所以 `post()` 只在「自增 + 条件唤醒」这条核心语义上对齐 POSIX,边界差异要分清。

## 生产者-消费者:把三件套拼起来

[main.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/main.cpp) 把 020 的「六个线程空转」demo 换成了生产者-消费者。全局三件套:

```cpp
static constexpr int PC_BUF_SIZE = 4;
static int g_pc_buf[PC_BUF_SIZE];
static cinux::proc::Semaphore g_sem_free(PC_BUF_SIZE);   // 空槽数, 初值 4
static cinux::proc::Semaphore g_sem_used(0);             // 货物数, 初值 0
static cinux::proc::Mutex g_pc_mutex;                    // 守缓冲
```

producer / consumer 的临界区用 RAII 包住,设计图画过,这里看写法:

```cpp
static void producer() {
    for (int i = 0; i <= 4; i++) {
        g_sem_free.wait();                  // 等空槽
        {
            auto g = g_pc_mutex.guard();    // 进临界区
            g_pc_buf[i % PC_BUF_SIZE] = i;
        }                                   // 出临界区, 析构解锁
        g_sem_used.post();                  // 发货物信号
        cinux::lib::kprintf("sent: %d\n", i);
    }
}
```

consumer 镜像:先 `g_sem_used.wait()`(等货),进临界区读,出临界区后 `g_sem_free.post()`(还空槽),打 `got: %d`。RAII `guard` 让临界区就是那对大括号——出括号自动 `unlock`,不用手写,也不会忘。`main` 里 `Scheduler::init()` 之后,用 `TaskBuilder` 各建一个 `producer` / `consumer` 任务、`add_task`,然后 `run_first`。

得诚实标注一处:在这个规模(缓冲 4、各发 5 个)下,**producer 大概率不会真的阻塞**。初始 `g_sem_free = 4`,producer 前四次 `wait()` 都让 `count_` 从 4 一路减到 0,每次都 `>= 0` 直返,根本没睡。只有第五次(占第五个槽)会让 `count_` 变 `-1` 而阻塞——前提是 consumer 还没来得及 `post` 任何空槽。在单核、`sti` 后时钟一打就切走的节奏下,producer 和 consumer 往往交错得很快,producer 那第五次 `wait` 很可能 consumer 已经 `post` 过空槽,于是又不阻塞。所以 demo 的串口输出 `sent: 0..4` / `got: 0..4` 验证的是「模型拼对了、没死锁、计数对得上」,**不**保证每次都真走阻塞路径。真正的阻塞语义——`wait` 到负、`post` 唤醒、FIFO 顺序——是靠下面那两组测试(host + QEMU 机内)盯死的,demo 只是端到端的烟雾测试。
