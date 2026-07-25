---
title: 01 · 导引:点亮什么、为什么、设计图
---

# 导引:点亮什么、为什么、设计图

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
