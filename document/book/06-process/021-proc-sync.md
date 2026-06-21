---
title: 021 · 让任务睡得下去、醒得过来:内核同步原语
---

# 021 · 让任务睡得下去、醒得过来:内核同步原语

> 上一章(020)我们让时钟来点名了——`tick`/`schedule` 把切换挂到了 IRQ0 上,`block`/`unblock` 这两个 API 也顺手写了,可写完就再没人用过:六个线程只是在各自空转打日志,谁也不碰谁的数据。这一章就把那两个落灰的 API 接成真正的同步原语。具体说,我们把 020 那个只在头文件里「定义着、没人用」的内联 `Spinlock` 搬成 out-of-line 实现,并加上内存序;再新写 `Mutex`(阻塞式互斥,靠等待队列让竞争者睡下去、`unlock` 时把所有权直接交接给队首)和 `Semaphore`(计数信号量,负的 `count` 直接表示「有多少人在排队」)。最后用一个生产者-消费者 demo 把三件套拼起来——一个 `free` 信号量数空槽、一个 `used` 信号量数货物、一把 `Mutex` 守共享缓冲,串口上看到 `sent: 0..4` / `got: 0..4`。做完,内核线程就有了「等」的能力,不再只能一味空转。顺带,这一章还会捎上一个挺魔幻的副作用:多加一个 `sync.cpp`,竟然让大内核进不去——根子在汇编器对 `mov rsp, imm` 的两种合法编码,我们调试现场再拆。

## 这一章我们要点亮什么

核心一句话:**让任务在拿不到资源时睡下去、在资源到位时醒过来**,而不是忙等。

拆成三块交付,外加一条贯穿始终的纪律:

- **Spinlock 升级成 out-of-line**:`acquire()` / `release()` 从 020 那种「直接写在头文件里」搬进 `sync.cpp`,内容不变但落了地——`__atomic_test_and_set(&locked_, __ATOMIC_ACQUIRE)` 在 `while` 里自旋,每次失败插一条 `pause`;`__atomic_clear(&locked_, __ATOMIC_RELEASE)` 释放。这两条内存序各自锁住临界区的入口与出口。
- **Mutex(全新)**:带 `owner_` 持有者指针 + FIFO 等待队列(单链表 `wait_head_`),内部用一把 `Spinlock` 只在操作等待队列的几行里短暂持有。`lock()` 空闲则记 owner 返回,竞争则入队尾,然后**先 `spin_.release()` 再 `Scheduler::block(self, "mutex")`**(这个顺序是铁律,反了就死锁)。`unlock()` 不清空 owner,而是把所有权**直接交接**给队首 waiter(把 `owner_` 设成 waiter 再 `unblock`)。配 `try_lock()` 不阻塞、`[[nodiscard]] guard()` 给 RAII。
- **Semaphore(全新)**:经典计数信号量,`count_` + FIFO 等待队列。`post()` 先 `count_++` 再 dequeue 队首、释放自旋锁后 `unblock`;`wait()` 先 `count_--`,`count_ >= 0` 直接返回,否则入队尾、释放锁、`block`。负数 `count_` 的绝对值,就是正在阻塞的等待者数——这是个非常省心的不变式。

贯穿三块的一条纪律:**自旋锁绝不能跨阻塞持有**。这是本章所有「先 release 再 block」顺序的统一根因,也是单核下这套原语能成立的前提。

边界得摆正,免得读者高估了这一章的能力:021 的同步原语**只服务内核线程**——`Task::addr_space` 在 demo 里压根没填,没有用户态、没有 ring3、没有系统调用。它是**单核**实现(`g_per_cpu` 是一个静态全局 `PerCPU`),`acquire()` 没有关中断,正确性靠的是「自旋锁绝不跨阻塞持有」加上单核抢占时机受限;一旦上多核、或者中断 handler 里真去动这些数据,这里的安全缺口就会暴露出来——那是留给以后的事。另外,`sync.hpp` 只有 Spinlock / Mutex / Semaphore 三件套,**没有**优先级继承、递归锁、读写锁、条件变量、屏障;Mutex 非递归,`owner_` 只是个单指针。还有一处特别容易误会:生产者-消费者 demo 在「缓冲大小 4、各发 5 个」的规模下,producer 大概率一路 `free.wait()` 不阻塞(初始 `free=4`),真正触发阻塞路径靠的是 QEMU / host 测试用例,正文讲 demo 时会如实标注这一点。

## 为什么现在需要它

020 的 demo 是六个线程各自空转打 `[A] iteration N`,它们**没有共享数据**。调度器保证它们被时钟交错打断,但谁也不等谁、谁也不抢同一个缓冲区——所以那套系统里,「同步」这件事其实没有需求。可一旦多个任务要碰同一块缓冲区、同一个结构体,两件事立刻冒头:**互斥**(同一时刻只能有一个任务在里面写)和**同步/通知**(「我写好了,你可以读了」)。020 的 `Spinlock` 能勉强顶第一件,但顶不了第二件;而且顶第一件也顶得很难看。

先说为什么单纯一把 `Spinlock` 不够。自旋锁的本质是「拿不到就原地打转」(`while (test_and_set) pause`)。这在持锁者很快会放手的前提下没问题。可如果持锁者**自己也要让出 CPU**——比如它拿了锁之后去等一个慢设备、或调了一个会阻塞的函数——在 020 这种抢占式系统里,持锁者会被时钟切走,而那个抢锁的任务还在原地 `pause` 死等。它占着 CPU 空转,持锁者却根本没在跑,谁也释放不了锁——这叫「自旋锁持锁者被抢占」,是自旋锁最经典的崩法。解法是让等待者**睡过去**而不是空转:拿不到锁就把自己挂到这个锁的等待队列上、调 `block` 让出 CPU;等持锁者 `unlock` 时,从队列里挑一个唤醒。这正是 `Mutex` 的职责——它把「等」从忙等变成了睡眠等待,持锁者被切走期间,CPU 可以去跑别的事,而不是被一个空转的等待者白白占住。

那为什么还需要 `Semaphore`?因为互斥只解决「我进去了你别进」,解决不了「我生产了一件货、你得知道」。生产者-消费者模型里,consumer 得等「缓冲里有货」才能读,producer 得等「缓冲有空槽」才能写——这是「等一个计数到正」,不是「等一把锁空出来」。`Semaphore` 正是为此而生:`count_` 表「可用资源数」,`wait()` 拿一个、拿不到就睡,`post()` 放一个、放出来就去唤醒一个等待者。两个信号量 `free`(空槽数)和 `used`(货物数)对偶,加一把 `Mutex` 守临界区,就是生产者-消费者的标准解。

最后回答一个看 `Semaphore::wait()` 时一定会问的:为什么允许 `count_` 变成负数?因为这样一条不变式直接成立——「负数 `count_` 的绝对值 = 正在阻塞的等待者数」。`wait()` 一进来就 `count_--`,先扣后判:扣完还 `>= 0`,说明资源有剩,直接走人;扣完 `< 0`,说明资源已经被前面的人耗光、连本次都透支了,那自己就是那 `-count_` 个等待者之一,入队、睡。等别人 `post` 把 `count_` 加回 0,正好对应「最后一个等待者被唤醒」。这个写法比「先判 `count_ > 0` 再扣」省一个分支,也避免了一种微妙的丢唤醒:扣是原子的(在自旋锁保护下),判和入队也在同一把锁里,`post` 的 `count_++` 和 dequeue 同样在这把锁里——自旋锁串起了 `count_` 和等待队列的修改,谁也不会在中间被打断。

## 设计图

先看 Mutex 的状态机——`owner_` 指当前持有者,`wait_head_` 是等待队列头,关键顺序标在图里:

```text
   Mutex  { spin_, owner_, wait_head_ }

   lock() (当前任务 self = g_per_cpu.current)
     ┌─ spin_.acquire()                              # ① 拿内部自旋锁: 只为动队列/owner
     │
     ├─ owner_ == nullptr ?                           # ② 空闲?
     │     是 → owner_ = self; spin_.release(); return   #   记下自己, 释放锁, 直接走人
     │     否 ↓                                       #   被占, 竞争
     │
     ├─ enqueue_waiter(self)                          # ③ 自己挂到队尾 (FIFO)
     ├─ spin_.release()                               # ④ 释放自旋锁  ◄── 铁律: 必须在 block 之前
     └─ Scheduler::block(self, "mutex")              # ⑤ 睡过去 (block 内部会 schedule 切走)
                                                      #    醒来时已被 unlock 标成 Ready, 下一轮被选中
   unlock() (由当前 owner 调)
     ┌─ spin_.acquire()
     ├─ waiter = dequeue_waiter()                     # ① 摘队首
     │     空 → owner_ = nullptr; spin_.release(); return   #   没人等, 清空 owner
     │     有 ↓
     ├─ owner_ = waiter                               # ② 直接把所有权交给 waiter (不清空!)
     ├─ spin_.release()                               # ③ 释放自旋锁
     └─ Scheduler::unblock(waiter)                    # ④ 唤醒新 owner (Ready + enqueue)
```

这张图里有两处顺序是命门,改一行就出 bug。一处是 `lock()` 的 ④→⑤:**释放自旋锁必须在 `block` 之前**。`Scheduler::block(self, ...)` 内部会判 `if (task == current_) schedule();`,也就是当前任务阻塞时要 `schedule()` 切走别人;如果此刻自旋锁还攥在手里,被 `schedule` 切进来的下一个任务只要也想碰这把锁(或任何用到自旋锁的路径),就死锁。另一处是 `unlock()` 的 ②:**不是把 `owner_` 清空、而是直接交接给 waiter**。要是清空 owner 再 `unblock`,在这两者之间万一有第三个任务来 `lock()`,它会看到 `owner_ == nullptr` 而直接抢走——刚被唤醒的那个 waiter 醒来发现自己排在了一个已经被别人插队的锁后面,等于丢了一次唤醒。直接把 owner 设成 waiter,锁的所有权从不悬空,醒来的人手里就有锁。

再看 Semaphore 的 `count_` 演化,把「负数 = 等待者数」这条不变式画清楚:

```text
   Semaphore { spin_, count_(int64_t), wait_head_ }

   wait():  count_--  先扣, 后判   (在自旋锁保护下)
     count_ == 2 ──wait──► 1   (>=0, 直返, 不睡)
     count_ == 1 ──wait──► 0   (>=0, 直返, 不睡)
     count_ == 0 ──wait──► -1  (<0, 入队尾, release 锁, block)   ← 1 个等待者
     count_ ==-1 ──wait──► -2  (<0, 入队尾, release 锁, block)   ← 2 个等待者

   post():  count_++  先加, 再 dequeue(锁内) → release 锁 → unblock(锁外)
     count_ ==-2 ──post──► -1  摘队首 waiter 唤醒   (仍有 1 个等待者)
     count_ ==-1 ──post──►  0  摘队首 waiter 唤醒   (等待者清零)
     count_ == 0 ──post──►  1  队列空, 不唤醒       (资源存进 count_ 等下次 wait)

   try_wait(): count_ > 0 ? count_-- 返回 true : 返回 false (不睡)
```

注意 `post()` 的「先 `count_++` 再 dequeue」:`count_` 加完如果还是负数,说明唤醒完仍有等待者没轮上;如果加到 `>= 0`,dequeue 会拿到队首那个等待者唤醒它。资源(正的 `count_`)和等待者(负的 `count_`)通过这个正负号天然分流——`count_` 既记资源余量,又记等待者欠债,一个字段两用。

最后是生产者-消费者的时序,把三件套的职责切开:

```text
   共享: int g_pc_buf[4]; Semaphore g_sem_free(4); Semaphore g_sem_used(0); Mutex g_pc_mutex;

   producer(i):                         consumer(i):
     g_sem_free.wait()   ① 等空槽         g_sem_used.wait()   ① 等货物
     { g = g_pc_mutex.guard() ② 进临界区    { g = g_pc_mutex.guard() ② 进临界区
       g_pc_buf[i%4] = i   写                 val = g_pc_buf[i%4]  读
     }                                  }                                   ③ 出临界区(析构解锁)
     g_sem_used.post()   ③ 发货物信号     g_sem_free.post()   ③ 发空槽信号
     kprintf("sent: %d\n", i)           kprintf("got: %d\n", val)
```

`free` 初始 4 对应 4 个空槽,`used` 初始 0 表示一开始没货。producer 每发一件:`free--`(占一个空槽)、写缓冲、`used++`(放一件货);consumer 每收一件:`used--`(取一件货)、读缓冲、`free++`(空出一个槽)。两把信号量把「缓冲满了 producer 得等」「缓冲空了 consumer 得等」管死,`Mutex` 只守那两行真正的临界区。RAII `guard()` 让临界区出大括号就解锁,写起来和 `std::lock_guard` 一样省心。

## 代码路线

### Spinlock:为什么从内联搬出来,以及内存序

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

### Task::wait_next:免堆分配的侵入式等待队列

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

### Mutex:阻塞式互斥与所有权交接

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

### Semaphore:计数信号量,负 count 即等待者数

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

### 生产者-消费者:把三件套拼起来

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

## 调试现场

这一章只有一条真实笔记,但它是个典型的「改一处不相关的代码、炸一处完全不相关的检查」,值得当案例。

### 案例:加 sync.cpp 让大内核进不去——mov rsp 的两种编码

症状挺唬人:`make run-kernel-test` 跑 mini kernel 测试,一路绿,打印 `=== MINI KERNEL TESTS PASSED ===`,然后突然冒出一行 `=== Loaded ELF is not a real kernel, exiting ===`,big kernel 测试**根本没执行**就退出了。mini 全过、big 没进,中间断在一个「入口魔数检查」上。

这个检查在 [main_test.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/test/main_test.cpp)。mini kernel 跳进 big kernel 之前,要确认入口处确实是「真内核」——它靠看入口字节:`_start` 的前两条指令是 `cli`(`0xFA`)+ `mov rsp, $__kernel_stack_top`。旧检查只认一种编码:

```cpp
// 旧: 只认 imm64 编码
bool is_real_kernel = (code[0] == 0xFA) && (code[1] == 0x48) && (code[2] == 0xBC);
```

`mov rsp, imm` 在 x86-64 有两种合法编码。`mov r64, imm64` 编码成 `48 BC <8 字节立即数>`(REX.W + 全宽立即数);而 `mov r/m64, imm32` 编码成 `48 C7 C4 <4 字节立即数>`(REX.W + 符号扩展的 32 位立即数)。GNU assembler 会看立即数范围自动选:当立即数(这里是 `__kernel_stack_top`)的低 32 位符号扩展后恰好等于 64 位值时,它就选更短的 `imm32` 编码(`48 C7 ...`),否则才用 `imm64`(`48 BC ...`)。

那为什么会突然换编码?根因正是本章那个看似无害的改动——把 `Spinlock` 从内联搬进 `sync.cpp`,等于多链了一个翻译单元,`sync.cpp` 里 `Mutex`/`Semaphore` 的静态成员、`g_pc_buf` 之类的全局让 **BSS 段长大了一点**。`__kernel_stack_top` 的链接地址随之微动,它的低 32 位恰好变成了「可以符号扩展」的那个范围,assembler 于是从 `48 BC`(imm64)换成了 `48 C7 C4`(imm32)。实际入口字节成了 `FA 48 C7 C4 00 00 02 81 ...`,而旧检查的第三个字节只认 `0xBC`,碰到 `0xC7` 就判「不是真内核」,直接退出。

修复是把检查放宽,两种编码都认:

```cpp
bool is_real_kernel = (code[0] == 0xFA) && (code[1] == 0x48) &&
                      (code[2] == 0xC7 || code[2] == 0xBC);
```

防复发的教训,比 bug 本身更值钱:任何基于机器码字节模式的检查,都必须**枚举所有等价编码**。x86-64 的 `mov` 立即数加载就有两种、assembler 还会按立即数范围自动挑最短的——链接地址哪怕动一个字节(而 BSS 段大小的任何变化都会动链接地址),都可能让 assembler 换一种编码,把你那套只覆盖一种情况的字节检查直接打穿。更稳的做法是不依赖具体编码、改成校验入口结构(比如反汇编一条 `cli` + 一条写到 `rsp` 的 `mov`),但在 mini kernel 这种连反汇编器都没有的早期环境里,「枚举等价编码」是最务实的一档。

## 验证

这套同步原语有两层测试,一层在 host、一层在 QEMU 机内,互为补充。

host 侧,[test_sync.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_sync.cpp) 把 `Spinlock`/`Mutex`/`Semaphore`/等待队列的逻辑**纯重实现**了一份(不链内核代码,用 `std::atomic` 代替 `__atomic`,mock 掉 `Scheduler::block`/`unblock` 和 `g_per_cpu`),`CINUX_HOST_TEST` 门控。它盯的全是「逻辑对不对」:`Spinlock` 初态/acquire-release/guard/double-release 良性、等待队列 enqueue/dequeue/FIFO/空队/清 `wait_next`、`Mutex` 设 owner/清 owner/`try_lock` 成功失败/竞争 block+入队/所有权交接/三等待者 FIFO/RAII、`Semaphore` 初值/默认 0/`post++`/`wait` 正数不阻塞/`wait` 到负阻塞/`try_wait` 成功失败/边界(大初值、repeated post)/lock-unlock 复用、`Task::wait_next` 零初始化为 null:

```bash
ctest --test-dir build -R sync --output-on-failure
```

真正的原子操作(`__atomic_test_and_set`)和真正的 `Scheduler::block`/`unblock`(真调度器、真状态流转)只能在 QEMU 里验。[test_sync.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_sync.cpp) 跑在真 PMM/VMM/Heap 之上,节名 `Sync Tests (021)`,入口 `run_sync_tests()`:

```bash
cmake --build build --target run-kernel-test
```

机内会跑九组:`Spinlock` acquire+guard;`Mutex` lock/`try_lock` free/held、竞争 block+入队 / unlock 交接 / 三等待者 FIFO、RAII;`Semaphore` 初值/默认 0/`post++`/`wait` 正数/`wait` 到 0 阻塞(`count`→-1)、`try_wait` 成功/0 失败/耗尽、`post` 唤醒 / 三等待者 FIFO、生产者-消费者计数模型(无阻塞,验证计数对得上)、`Task::wait_next` build 后为 null。这组测试是阻塞语义的真正保证——demo 不一定触发的阻塞路径,在这里被构造性覆盖。

最后是生产 demo,跑大内核看端到端:

```bash
cmake --build build --target run
```

串口应该看到 producer / consumer 交替输出 `sent: 0` ... `sent: 4` 与 `got: 0` ... `got: 4`,不死锁、不丢数。如前所述,这个规模不保证每次都真走阻塞,它验证的是「三件套拼对了、计数对得上」,不是「阻塞路径每次都触发」——后者归 QEMU 机内测试管。

## 下一站

021 让任务「睡得下去、醒得过来」,内核线程第一次有了真正的同步原语。可这套原语的所有前提都还停在内核态、单核、不关中断:`Spinlock` 的 `acquire` 没有关中断,正确性靠「自旋锁绝不跨阻塞持有」加单核抢占时机;`Mutex`/`Semaphore` 的 `block` 走的是 020 那条软件调度路径,和中断使能无关;`Task::addr_space` 在 demo 里压根没填,所有线程共享内核地址空间。

下一站(022)要跨出这一步:进 ring3,造用户态进程、系统调用,于是 SFMASK、MSR、中断门改 IF 这些会被重新审视——那一章会反过来拷问本章的同步原语:「在用户态可被打断、在中断里可能重入的世界里,这把自旋锁还安全吗?」本章留下的多核/IRQ 安全缺口,要往真正的可抢占方向推,就得先把 ring0 内核线程和 ring3 用户进程的边界划清楚。021 的 Spinlock/Mutex/Semaphore 是那条边界上一旦跨过去就要重新加固的地基——原语先立住,边界后划清。

---

### 参考

- **GCC `__atomic` Builtins**(`https://gcc.gnu.org/onlinedocs/gcc/_005f_005fatomic-Builtins.html`,在线 200):`__atomic_test_and_set`(原子置 1 并返回旧值)、`__atomic_clear`(原子清 0)、`__ATOMIC_ACQUIRE`/`__ATOMIC_RELEASE` 的 happens-before 语义。支撑本章 `Spinlock::acquire`/`release` 的实现与内存序配对,延续 020 章已核引用。
- **POSIX `sem_post(3)`**(`https://man7.org/linux/man-pages/man3/sem_post.3.html`,man-pages 6.18,已读正文):「increments (unlocks) the semaphore;若结果 > 0 则唤醒一个 `sem_wait` 阻塞者」、`MT-Safe`、`async-signal-safe`。支撑 `Semaphore::post()` 的设计锚点,以及正文「`SEM_VALUE_MAX`/async-signal-safe 本章未实现」的边界对比。
- **Intel SDM Vol.2B `PAUSE` 条目**(本地 `document/reference/intel/SDM-Vol2B-Instruction-Reference-M-U.pdf`):`pause` 作为 Spin-Wait Hint 的概念性依据;手册内具体页未在本地 PDF 定位到,故正文仅作概念描述、不引页码。
- **OSDev Wiki "Spinlock" / "Semaphore"**(`https://wiki.osdev.org/Spinlock`、`https://wiki.osdev.org/Semaphore`):test-and-set + `PAUSE` 朴素自旋锁、计数信号量 + 有界缓冲生产者-消费者的社区路径,概念性对照(域名 403 反爬,无法抓正文,仅作方向引用)。
- **020 章 · [时钟到点,该换人了:抢占式调度](020-proc-scheduler.md)**:`Scheduler::block(Task*, const char*)` / `unblock(Task*)`、`g_per_cpu.current`、`Spinlock` 原语与「自旋锁只定义、没人用」的现状——本章直接接续并落地。
- 本 tag 源码:[sync.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/sync.hpp) / [sync.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/sync.cpp)、[process.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/process.hpp)(`Task::wait_next`)、[scheduler.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/scheduler.hpp) / [scheduler.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/scheduler.cpp)(`block`/`unblock`)、[per_cpu.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/per_cpu.hpp)、[main.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/main.cpp)(生产者-消费者 demo)、[main_test.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/test/main_test.cpp)(魔数检查双编码);测试 [test_sync.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_sync.cpp)(host 镜像)、[test_sync.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_sync.cpp)(QEMU 机内,节名 `Sync Tests (021)`)。
