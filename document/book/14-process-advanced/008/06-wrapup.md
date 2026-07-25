---
title: 06 · 验证、没做的、小结
---

# 验证、没做的、小结

## 验证:timer 被 084 间接证、stats kthread 靠运行曲线

这两个原语都属于「真调度难单测」一类——验证策略不同,但共同点是**不写直接单测、靠集成路径 + 运行观察**。这一节诚实标注边界,不拔高。

### timer_queue:靠 084 间接证,没有独立单测

grep `kernel/test/` 没有 `test_timer_queue.*`——整棵树只有 084 的 `test_poll_finite_timeout_parks_then_returns_zero`([test_poll.cpp:191-204](../../../kernel/test/test_poll.cpp#L191)):

```cpp
// kernel/test/test_poll.cpp:191-204
/// A finite-timeout poll on an empty pipe PARKS (registered on the read wait
/// queue + the timer-wake) and returns 0 once the timer fires -- the real
/// blocking path, not a yield spin.  Proves the timer-wake end-to-end.
void test_poll_finite_timeout_parks_then_returns_zero() {
    int rfd, wfd;
    make_pipe(rfd, wfd);

    uint16_t rv = 0xFFFF;
    int64_t  r  = poll_one_timeout(rfd, kPollIn, 30, rv);
    TEST_ASSERT_EQ(r, 0);  // nothing became ready -> timeout
    TEST_ASSERT_EQ(rv, 0);

    close_fds(rfd, wfd);
}
```

它 poll 一根空 pipe、超时 30ms、期望 `r==0` 且 `revents==0`。它没点名 `timer_queue`,但「30ms 后真能返回」**只可能是** timer_queue 在 tick 里唤醒了被 park 的任务——这是 timer_queue 端到端的活证据。整条链是:

1. `poll_one_timeout` 进 `do_poll_core` 的 park 块(`poll_core.cpp:180`)。
2. `InterruptGuard` 关 IRQ → `prepare_to_wait` 翻 Blocked → `register_all` 挂 read 队列 → `timer_queue_arm(self, deadline)`(`:188`)挂 30ms deadline。
3. `schedule_blocked`(`:204`)真切走。
4. PIT 驱动的 `Scheduler::tick`(`scheduler.cpp:367` 定义)每次中断在 `:379` 调 `timer_queue_tick`,30ms 后扫到 deadline 过期,把 task 收进 `fired[]`、锁外 `Scheduler::unblock`。
5. poller 醒来,`detach_all` 撤销 fd 注册、`timer_queue_disarm`(`:207`)清自己的槽位。
6. 回 for 顶 Pass 1 重扫,空 pipe 没就绪、deadline 已过,返回 0、revents=0。

这条链跑通 = timer_queue 端到端验过。所以本章引用 timer_queue 时标「被 084 间接验证」,不是「有单测」——这是诚实的措辞。

### stats_kthread:靠 ON + make run + 串口曲线,没有单测

stats_kthread 依赖真调度器循环 + 真时间流——yield 让 CPU、PIT tick 切回、HPET 判 1s。单测要造真调度器 + 时间快进,工程上不值。验证路径是运行态:

1. `cmake -DCINUX_STATS_KTHREAD=ON` 配 make。
2. `make run`,跑真实 workload(典型 buildroot rootfs 里 `g++ hello.cpp`)。
3. 看串口每隔 ~1 秒出一组 `[MEM] === t=N.NNN s ===` + PMM/Slab/PageCache/#PF/I/O 五行。

CinuxOS 开发笔记给过一条 31s g++ 编译曲线就是这种验证的产物——PMM 始终 ~2.08M 页(8GB 充裕,sec 0 `2088396` / sec 30 `2086187`)、Cache 才涨到 1767 页(~7MB,sec 30)、PF 爆发集中在 sec 8/13/20(对应 cc1/cc1plus 加载大库,sec 20 `+18272` 是峰值),诊断结论是「内存子系统不是瓶颈」,卡顿要往 QEMU TCG 翻译开销 / disk I/O / 热 syscall 找。(那条曲线是 ext2 I/O 行还没加进 dump 之前采的——当时每次 dump 4 行 [MEM];当前源码每次 dump 6 行,同样 31s 复现会是 ~186 行。但 per-sample 的数字——PMM free、Cache、PF total、PF delta——不受每 dump 行数影响,仍对得上。)

**注意**:默认 OFF 跑过 2290 PASS / 0 FAIL(dev note 数据)只验「gate 不破坏 kernel」(stub 链零回归),**不验「kthread 真能周期采样」**——后者要 ON + make run。这两条要分开标,别把前者当后者。

还有个容易混的点:`test_memory_stats.cpp`([test_memory_stats.cpp:21](../../../kernel/test/test_memory_stats.cpp#L21)):

```cpp
// kernel/test/test_memory_stats.cpp:21
void test_dump_runs_and_reports() {
    dump_memory_stats();  // observed on serial; must not fault
    TEST_ASSERT_GT(g_pmm.total_page_count(), 0ULL);
    TEST_ASSERT_GT(g_pmm.free_page_count(), 0ULL);
}
```

它只调一次 `dump_memory_stats` 断言不 fault、PMM total/free > 0。这是 FO(Foundation)基础设施的测试——测的是 `dump_memory_stats` 这个函数本身能跑、PMM 计数器有值——**不是 stats_kthread 的证据**。别把它当成「stats_kthread 有单测」。

### 两条 gate 分开

这两个原语的 gate 状态也不一样,要分清:

- **timer_queue**:没 gate,永远编进 kernel(`timer_queue.cpp` 在 `kernel/proc/CMakeLists` 里无条件链),被 084 poll 吃。
- **stats_kthread**:§14 file gate,默认 OFF。OFF 时链 stub,ON 时链真实现。

## 这章没做的

诚实的边界清单:

1. **没在本机跑 `cmake -DCINUX_STATS_KTHREAD=ON` + `make run` 复现 31s 曲线**——曲线数字(2088396 / 1767 / 31117 / +18272)和 2290 PASS/0 FAIL 全部来自 CinuxOS dev note。本章只核实 Book 侧源码接线与该 note 描述一致——文件齐全、行号对得上、调用链闭合。读者若在本机 ON + make run,数字会因 rootfs / QEMU 配置略有差异。
2. **没核 timer_queue 的 SMP 安全细节**——表是全局单例 `g_entries[32]`,AP 也调 `Scheduler::tick`(AP 起来后同样走 tick 路径),多核 tick 都拿同一 `g_lock` spinlock,理论安全,但没压力测试。084 的单次 30ms 超时不触发 `kMaxTimers` 表满、不触发多核并发 arm 等边界。
3. **timer_queue 的并发正确性(锁顺序、IRQ safety)是基于源码读出的论断,无压力测试佐证**——「timer_lock → run-queue lock 绝不反着」这条纪律是源码注释和实现结构推出的,没有专门的并发测试守住。
4. **stub 路径下 PF counter / dump #PF 行「无条件编」基于 CMakeLists 注释**——`diagnostics.cpp` 确实无 gate(全文读了一遍没 `#ifdef`),`page_fault.cpp` 只读了 `:55-89` 区段确认 `g_pf_count` 和 `handle_pf` 的原子操作,没逐行扫整个文件确认完全没有 `#ifdef` 包裹(但从结构看,PF 计数是 fault handler 的基础逻辑,被 gate 反而不合理)。
5. **nanosleep 未迁仍 yield-poll**——`timer_queue.hpp` 头注释([timer_queue.hpp:5-10](../../../kernel/proc/timer_queue.hpp#L5))提了 nanosleep 是 timer_queue 的目标用户,但 `sys_nanosleep.cpp:50-55` 还在 yield 自旋。**以 `.cpp` 为准**,nanosleep 这条现在其实没生效——`timer_queue` 真正的用户是 084 poll。
6. **stats_kthread 没用 timer_queue**——它是更早的 yield-poll 范式,band-0 旁观采样用 `yield` 同时承担「让 CPU 给被测对象」和「等 tick」两职责,park 反而不合适(会睡死、错过采样窗口)。这是两套不同场景的定时范式,本章只点出区别,不展开 timer_queue 改造 stats_kthread 的可行性(那是个 DEBT,没人做过)。
7. **HPET 依赖**——不可用时 stats_kthread 退化成数 tick(`++ticks_since_dump >= 100`),精度从 ns 掉到 ms 级(实际是 ~1 秒但抖动大)。本章不展开 HPET 不可用场景的实测。
8. **`dump_memory_stats` 的 `static last_pf`/`last_io` 依赖「无并发调用者」不变量**(panic 一次 + 单 stats 线程)——这条不变量目前成立,但如果哪天加了第二个调用者就 race。本章只标注,不改造。

## 小结

这章核实了两件内核基础设施在 Book 真能用,主题是「教程即验证」。

**timer_queue** 补的是调度器「到点自动唤醒」缺失的半边——084 poll 用它的 arm/disarm 接口真 park 了一个有限 timeout 任务。内部是一个容量 32 的固定表,线性扫,O(n) 在 PIT tick 频率下成本可忽略,换来「无堆下滤、无树旋转」的结构平凡正确。三个接口:`arm` 关 IRQ 拿锁、先替换已有 arm、再找空槽、表满返 false 让调用方降级;`disarm` 醒来清槽防 stale;`tick` 由 `Scheduler::tick` 每次中断调,锁内收集过期 task 到栈上 `fired[]`、锁外才 `unblock`——这条锁外唤醒是 timer_queue 唯一真正值得学的并发点,锁序纪律 timer_lock → run-queue lock 绝不反着,否则 AB-BA 死锁。

**stats_kthread** 是周期采样的常驻 kthread,每秒 `dump_memory_stats` 打串口曲线。本章的明星是它的调度难点——围绕「不饿死、不扭曲、不垄断 CPU」三个角的不可能三角,两版实测失败 + 一版逻辑排除推出来的正解:

- priority 250 → 观察者饿死在被观察者手里,0 行 dump;
- priority 提高 → 抢占 user code 扭曲测量,自欺欺人;
- band 0 但 sti/hlt → tick 唤醒后 quantum 没耗尽继续选自己,init/child 永远 Ready,gate 卡在第一次 fork、#PF 全程 +0(与 071 章 sti/hlt 坑同族,机制不同:071 是陷阱帧损坏 #DF,这里是同级任务饿死)。

正解是 band 0 + yield——「醒得很勤(约 100Hz)但干活很稀(1Hz dump)」的两级节流,`yield` 同时承担「让 CPU 给被测对象」和「等下一个 tick」两件事。这是协作式/带优先级调度器里后台观测线程的正确姿势。

**§14 file gate** 把 gate 放在「整个周期线程」这一层——ON 链真实现、OFF 链空 stub,源码零 `#ifdef`,`init.cpp` 无条件调 `start_stats_thread()`,读起来和普通函数调用一样干净(和 `usb_stub`/`tlb_drain_stub` 同一套模式)。更细的一点:PF 计数器和 dump 的 #PF 行是无条件编的——panic 时也能看 #PF——只有「周期采样」这个 ad-hoc profiling 增强被 gate。

验证边界诚实标:timer_queue 没有独立单测,靠 084 的 `test_poll_finite_timeout_parks_then_returns_zero` 间接证 arm→park→tick 唤醒→disarm 整链;stats_kthread 没有单测,靠 ON + make run + 串口曲线(CinuxOS dev note 的 31s g++ 曲线是实测,Book 本地未重跑;dev note 的「126 行」是 ext2 I/O 行还没加进 dump 之前的 4 行/dump 计数,当前源码每次 dump 6 行,同样 31s 复现应是 ~186 行);`test_memory_stats.cpp` 测的是 `dump_memory_stats` 这个 FO 组件不是 kthread 本身。两条 gate 也分开——timer_queue 没 gate 永远编,stats_kthread 是 §14 file gate 默认 OFF。这些都不拔高,如实说。
