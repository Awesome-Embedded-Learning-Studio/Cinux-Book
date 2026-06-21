---
title: Lab 028d · 并发安全:亲手感受数据竞争,学会「该用哪种锁」
---

# Lab 028d · 并发安全:亲手感受数据竞争,学会「该用哪种锁」

> 028d 是一次加固,不是新功能——所以这个 lab 的重心不在「写一个新模块」,而在三件事:**第一**,在 host 上用真多线程亲手「看见」一次数据竞争,再用自旋锁把它治住,建立体感;**第二**,练出本章最值钱的判断力——给定一段临界区,该用 `guard()`、`irq_guard()`、`InterruptGuard` 还是原子,并说出「因为它(不)和中断共享」;**第三**,读懂生产 stress 测试,手算预期数字,跑 `run-stress-test` 看它是否精确命中。最后留一个扩展:给目前「造好却没上岗」的 `Mutex`/`Semaphore` 补一个真并发用例。

## 实验目标

- 在 host 上用 `std::thread` 复现「计数器丢更新」的数据竞争,再用 `Spinlock` 保护后确认计数精确,理解 test-and-set + pause 在干什么。
- 把「这块数据会不会被中断处理路径碰到」变成条件反射:给定若干 028d 临界区,正确分类该用哪种原语并给出理由。
- 手算 stress 测试的预期操作数(4 × 200、4 × 200、4 × 1000),跑 `run-stress-test` 对照 `expected=actual … PASS`。
- 用 `run-kernel-test` 验证 `InterruptGuard`/`IrqGuard` 的关中断、恢复、嵌套行为在真硬件上正确。
- (扩展)检查 host 并发测试对 `Mutex`/`Semaphore` 的覆盖,补一个真并发用例。

## 前置条件

- 028c 通过(`ctest -R cwd_stat` 能跑)。
- 028d 代码已构建:`cmake --build build`。
- host 工具链支持 `-pthread`(Linux 一般自带)。
- 读懂主书第 028d 章的「四个同步原语」「三层加固落地」「设计现场 A/B」三节。

## 任务分解

### 任务 1:在 host 上亲手看见一次数据竞争

`test/unit/host_spinlock.cpp` 是 028d 专门加的 host 实验:它把内核的 `Spinlock`(在 `CINUX_HOST_TEST` 下编进 host)拿到 `std::thread` 环境里压。先做对比实验,建立体感——下面只给骨架,不给完整程序,你自己补全跑通:

```text
// 对照 A:多个线程对一个 volatile 计数器「裸自增」N 次
//   预期:总和 < 线程数 × N(几乎必然丢更新)
// 对照 B:把自增用 Spinlock::guard() 包起来
//   预期:总和 == 线程数 × N(精确)
```

跑对照 A 多次,你会发现每次的「丢失量」都不一样——这正是数据竞争「偶发、不可复现」的嘴脸。然后上锁,丢更新消失。

要留意的点:

- `Spinlock` 在 host 下用的是同一份 `kernel/proc/sync.cpp`?还是 host 提供一个等价实现?读 `host_spinlock.cpp` 的 include 和条件编译搞清楚——这关系到你在 host 上压的到底是不是内核那把锁。
- 把线程数和每线程自增次数调大(比如 8 线程 × 100 万),对照 A 的丢失量会更明显、更稳定。
- 体会 `pause` 的作用:它不改变正确性,只影响自旋时的效率与功耗。host 上感知不强,但在内核里挡的是「memory-order 违规重罚」。

### 任务 2:判断题——这段临界区该用哪种原语

这是本章最该带走的能力。判断标准只有一句:**这块数据,中断处理路径会不会碰它?** 会 → 关中断侧(`irq_guard` / `InterruptGuard` / 原子);不会 → 普通 `guard()`。

对下面每一段,判断该用 `guard()`、`irq_guard()`、`InterruptGuard` 还是 `std::atomic`,并写一句理由(主书「设计现场 A」是范本):

```text
(a) PMM::alloc_page 里的 find + set
(b) RoundRobin::pick_next 改运行队列
(c) PIT::irq0_handler 里自增 tick_count_(原子 `fetch_add`)
(d) Keyboard::poll 从环形缓冲取一个键
(e) sys_read 里 file->offset += result
(f) TaskBuilder 分配下一个 tid
```

参考答案的方向(自己先想再看):

- (a) `guard()`——只有内核线程碰 PMM,中断不碰。
- (b) `irq_guard()`——运行队列被 PIT IRQ0 的 `tick()→schedule()` 碰,**必须关中断**,否则持锁时被时钟中断重入会死锁。
- (c) `std::atomic`——单字段、被 IRQ0 超高频改,用锁太贵;`tick_count_` 是 `std::atomic<uint64_t>`。
- (d) `InterruptGuard`——环形缓冲与 IRQ1 ISR 共享,单生产者单消费者,只需临时关中断,不需要挡别的线程。
- (e) `guard()`——offset 是线程间共享,read/write 路径无中断碰。
- (f) `std::atomic`——`next_tid` 是分配号,单字段高频,原子即可。

如果你把 (b) 误判成 `guard()`,就去重读设计现场 A——那是这一章最硬的 why。

### 任务 3:手算 stress,再跑 stress

打开 `kernel/stress/stress_test.cpp`,先**别**跑,手算预期:

```text
NUM_THREADS     = 4
PMM_OPS         = 200   (每线程)
HEAP_OPS        = 200   (每线程)
shared 自增      = 1000  (每线程)

预期:
  pmm_ops_total   = 4 × 200 = 800
  heap_ops_total  = 4 × 200 = 800
  shared_counter  = 4 × 1000 = 4000
```

然后跑:

```bash
cmake --build build --target run-stress-test
```

对照输出里的三行 `expected=… actual=… PASS` 是否精确命中你算的数字。**重点想**:为什么 `actual` 能精确等于 `expected`?因为 PMM 加了锁(没有同一页分两次)、堆加了锁(free_list 没被改坏)、计数器是原子的(没丢更新)。如果哪个 `actual < expected` 或直接崩,就说明对应的那把锁没起作用——这是把「锁有没有生效」变成可观测数字的关键手段。

### 任务 4:在 QEMU 里验证 RAII 机制

```bash
cmake --build build --target run-kernel-test
```

在输出里找到 `Sync Concurrent Tests (028d)` 这一段(由 `run_sync_concurrent_tests()` 驱动)。确认它逐条通过,重点理解每条在断什么:

- InterruptGuard 进入后 `RFLAGS & 0x200`(IF 位)为 0,析构后恢复原值;嵌套时内层进出 IF 恒为 0、最外层才恢复。
- IrqGuard 持锁期间 IF=0;进入时 IF 本就是 0 的,析构后仍是 0(不误开中断)。
- Spinlock / IrqGuard 的协作式互斥:三个 Task 各自自增若干次,总和精确。

想清楚:这层为什么是「协作式」(手动切 `g_per_cpu.current`、顺序执行)而不是真并发?因为它要验的是「关中断/加锁/释放这些动作**本身**是否分毫不差」,机制正确性;真并发安全性交给任务 3 的 stress。两层各管各的。

### 任务 5(扩展):给 Mutex / Semaphore 补一个真并发用例

主书如实指出:028d 里 `Mutex` 和 `Semaphore` 只被定义和测试,**没有任何生产代码用它们**,生产防护全是 Spinlock/InterruptGuard/原子。先做调查:

```bash
# 确认 Mutex/Semaphore 在生产代码里的使用面
git grep -nE '\bMutex\b|\bSemaphore\b' -- 'kernel/*.cpp' 'kernel/*.hpp' \
    ':!kernel/proc/sync.cpp' ':!kernel/proc/sync.hpp' ':!kernel/test/*'
```

如果结果是空(生产代码里确实没用),再看看 host 端的并发测试(`test/unit/test_sync_concurrent.cpp`、`host_spinlock.cpp`)是否对 `Mutex`/`Semaphore` 有**真并发**(多 std::thread)覆盖。

你的任务:如果 `Mutex`/`Semaphore` 还缺真并发用例,补一个。两个经典场景,任选其实现:

- **Mutex 保护计数器**:N 个 `std::thread` 用 `Mutex::lock()/unlock()` 保护一个共享计数,断言精确。
- **Semaphore 生产者/消费者**:用 `Semaphore` 做一个有界缓冲的生产者/消费者,断言生产总数 == 消费总数。

实现时务必遵守主书「设计现场 B」的纪律:**`Mutex::lock()` 里 `spin_.release()` 必须在 `Scheduler::block()` 之前**——持着自旋锁去阻塞是死锁。在 host 上没有调度器,`block` 退化为空操作,所以这条纪律在 host 测试里看不出来;但你写用例时要心里有数,因为这条纪律在内核里是命门。

补完跑 `ctest -R sync_concurrent`,全绿。

## 接口约束

这一章的关键接口,你测的每个点都对应其中之一(签名以 `kernel/proc/sync.hpp` 为准):

- `cinux::proc::Spinlock::acquire()` / `release()`:test-and-set + pause,ACQUIRE/RELEASE。
- `cinux::proc::Spinlock::guard()`:[[nodiscard]] RAII,不关中断,挡线程。
- `cinux::proc::Spinlock::irq_guard()`:[[nodiscard]] RAII,关中断(RFLAGS.IF) + 自旋,挡线程也挡中断。
- `cinux::proc::InterruptGuard`:纯关中断 RAII,嵌套安全。
- `cinux::proc::Mutex::lock()` / `unlock()` / `try_lock()`:阻塞式,内部 Spinlock 只护 owner/wait queue;`release` 必须在 `block` 前。
- `cinux::proc::Semaphore::wait()`(P)/ `post()`(V)/ `try_wait()` / `count()`。
- `cinux::proc::Task::wait_next`:侵入式等待队列链(Mutex/Semaphore 复用)。
- `cinux::fs::File::offset_lock_`(`mutable Spinlock`):sys_read/write/getdents 持有。
- `cinux::fs::FDTable::lock_` / `cinux::mm::PMM::lock_` / `Heap::lock_` / `VMM::lock_`:各子系统 plain guard。
- `RoundRobin::lock_`:调度运行队列,**`irq_guard()`**。

## 验证步骤

- **任务 1**:补全 `host_spinlock.cpp` 的对照实验(或新建一个 host 测试文件加 `-pthread`),`ctest --test-dir build -R sync_concurrent --output-on-failure` 全绿;对照 A 能稳定看到丢更新、对照 B 计数精确。
- **任务 2**:纸上完成分类,每条配一句理由;重点 (b) 必须答 `irq_guard()` 并能解释「持锁时被时钟中断重入会死锁」。
- **任务 3**:手算数字写在纸上 → 跑 `run-stress-test` → 三行 PASS 的 `expected` 与你手算一致、`actual` 精确相等。
- **任务 4**:跑 `run-kernel-test`,`Sync Concurrent Tests (028d)` 段全绿,能解释「为何协作式」。
- **任务 5**:`git grep` 确认 Mutex/Semaphore 生产代码零使用;补的真并发用例 `ctest -R sync_concurrent` 全绿。
- 全程在干净构建上跑;stress 跑前确认是 4 线程配置(读 `stress_test.cpp` 顶部的常量,别拿旧数字套)。

## 常见故障

- **写 `lock.guard();` 一行,编译器告警**:这是 `[[nodiscard]]` 在救你——返回的临时 Guard 立刻析构,等于没加锁。必须 `auto g = lock.guard(); (void)g;` 把生命期续到作用域末。参见设计现场 C。
- **host 上裸自增「没丢更新」**:线程数太少或每线程次数太少,竞争窗口太小。调大到 8 线程 × 百万级;或加 `-O0` 避免编译器把自增优化成不可打断的指令。
- **手算 stress 数字和实际对不上**:你用了错的常量。`stress_test.cpp` 里 `NUM_THREADS=4`、`PMM_OPS=200`、`HEAP_OPS=200`、共享自增是硬编码的 `1000`,不是 `HEAP_OPS`。读源码顶部,别凭记忆。
- **以为 Mutex 已经保护了 PMM/堆**:没有。028d 生产防护全是 Spinlock。Mutex/Semaphore 造好了但没上岗——这是事实边界,别在报告里写错。
- **把调度器运行队列的锁写成 `guard()`**:会死锁。它被 PIT IRQ0 的 `tick()` 碰,必须 `irq_guard()`。这是任务 2(b) 的核心。
- **扩展任务里 Mutex 用例「偶尔挂」**:检查是不是在持锁状态下又触发了阻塞/调度。host 上 `block` 是空操作所以不显,但纪律要守:`release` 在 `block` 之前。

## 通过标准

- 任务 1 能稳定复现裸自增的丢更新,并解释 `Spinlock` 如何消除它;能说清 `pause` 不影响正确性、影响什么。
- 任务 2 六条分类全对,(b) 必须答 `irq_guard()` 并讲清死锁机理。
- 任务 3 手算的三个数字与 `run-stress-test` 输出精确一致,并能解释「`actual` 精确等于 `expected` 意味着哪些锁生效了」。
- 任务 4 `run-kernel-test` 的 Sync 段全绿,能解释「协作式机制测试」与「抢占式 stress」各管什么。
- 任务 5 完成调查(确认 Mutex/Semaphore 生产零使用)并补出真并发用例,`ctest -R sync_concurrent` 全绿。
- 能口头回答:为什么有两种 guard?调度器为什么必须关着中断加锁?为什么不能持着自旋锁去阻塞?
