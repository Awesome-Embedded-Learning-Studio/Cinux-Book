---
title: 02 · 同步原语:Spinlock、InterruptGuard、Mutex、Semaphore
---

# 同步原语:Spinlock、InterruptGuard、Mutex、Semaphore

## 先造基建:四个同步原语

在动手加固之前,得先有趁手的工具。008 新增的 `kernel/proc/sync.{hpp,cpp}` 一口气造了四个原语,分两层:

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

**但这里必须诚实一句**:在 008,`Mutex` 和 `Semaphore` **只被定义和测试,还没有任何生产代码用它们**。你 grep 整个 `kernel/`(排除 `sync.cpp`/`sync.hpp` 自身和测试目录)会发现一个 `.lock()` 调用都没有——这一章真正上线防护的,全是 `Spinlock`(`guard()` 或 `irq_guard()`)、`InterruptGuard` 和原子计数器。`Mutex`/`Semaphore` 是为后面那些需要「真 sleep」的场景(等磁盘、等信号、等条件)预备的弹药。我们仍然要讲它们的实现,因为里面有一条「release-before-block」的纪律太经典、现在就得讲明白——见设计现场 B。记住这个边界,免得你以为 008 已经用上了阻塞锁。

## 三层加固落地
