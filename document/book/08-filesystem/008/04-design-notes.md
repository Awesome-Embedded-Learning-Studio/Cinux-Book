---
title: 04 · 设计现场:调度器、release-before-block、键盘为什么关中断
---

# 设计现场:调度器、release-before-block、键盘为什么关中断

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
