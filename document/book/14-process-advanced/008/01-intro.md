---
title: 01 · 导引:点亮什么
---

# 导引:点亮什么

> 084 章你用 poll 有限 timeout 真睡了一个任务——`prepare_to_wait` 翻 Blocked、`timer_queue_arm` 挂一个 30ms 的 deadline、`schedule_blocked` 切走,30ms 后 timer tick 把它叫醒返回 0。那套用法的骨架是 084 章讲的;但 `timer_queue_arm` 内部到底挂了什么、谁负责到点叫醒、为什么不会和运行队列撞锁——那半边没讲。这一章就是把那半边翻给你看。
>
> 同一章还有第二件基础设施:`stats_kthread`,一个常驻内核线程,每秒把 PMM 余量 / slab 页 / PageCache 占用 / 累计缺页数打一次串口,画成一条曲线给「g++ hello.cpp 卡顿」这类 workload 做性能剖析。这玩意儿本身代码很短(95 行),真正值钱的是它的调度难点——它必须坐在 band 0(和 init、cc1 同一优先级档),既不能太低(会被编译饿死、采不到样)、也不能太高(会抢占 user code 扭曲测量数),还不能 `sti/hlt`(会和 071 章那个 sti/hlt 坑同族,把 gate 卡死在第一次 fork)。围绕这三个角的不可能三角——两版实测失败(priority 250 饿死、band 0 + sti/hlt 卡死)+ 一版逻辑排除(priority 提高会扭曲测量)——最后推出来的正解是 `yield`,一个同时承担「让 CPU 给被测对象」和「等下一个 tick」两件事的姿势。这一章把这条推理链完整走一遍。
>
> 章末是 §14 file gate:CMake 用一个 `CINUX_STATS_KTHREAD` option 选链 `stats_kthread.cpp`(真实现)还是 `stats_kthread_stub.cpp`(空函数),源码零 `#ifdef`,`init.cpp` 无条件调 `start_stats_thread()`。这是 Cinux 把可选增强收进编译选项的标准模式(usb_stub / tlb_drain_stub 同款)。

## 这章咱们要点亮什么

1. **timer_queue 的内部**:一个容量 32 的固定表 `Entry{Task*; uint64_t deadline_ns; bool armed}`([timer_queue.cpp:34](../../../kernel/proc/timer_queue.cpp#L34)),`Spinlock` 守护。三接口——`timer_queue_arm`(`:54`)关 IRQ 拿锁、先扫一遍替换已有 arm、再找空槽、表满返 `false` 让调用方降级成无 timeout park 绝不挂死;`timer_queue_disarm`(`:77`)醒来清自己的槽防 stale wakeup;`timer_queue_tick`(`:90`)由 PIT 驱动的 `Scheduler::tick`(`scheduler.cpp:367` 定义)每次中断在 `:379` 调,锁内收集过期 task 到栈上 `fired[]`、释放锁后才 `unblock`。这条「锁外唤醒」是 timer_queue 唯一真正值得学的并发点。
2. **为什么是固定表不是堆/红黑树**:头注释([timer_queue.cpp:5-9](../../../kernel/proc/timer_queue.cpp#L5))写明「a linear scan of kMaxTimers is cheap at hobby scale... and keeps the structure trivially correct」——32 槽远超实际并发定时等待数,O(n) 在 PIT tick 频率下成本可忽略,换来结构平凡正确(无堆下滤、无树旋转)。这是「结构平凡地正确」的支点。
3. **stats_kthread 是干什么的**:常驻 kthread,每 ~1 秒(1e9 ns)调一次 `dump_memory_stats()`([diagnostics.cpp:26](../../../kernel/mm/diagnostics.cpp#L26)),把 PMM/slab/PageCache/#PF/I/O 五行打到串口形成曲线。计时基准是 HPET free-running counter(`stats_kthread.cpp:46-51`),HPET 不可用时退化成数 tick(`++ticks_since_dump >= 100`,`:67-72`)。
4. **band 0 kthread 不能 sti/hlt——本章明星**:stats kthread 必须 band 0(低会被饿死、高会扭曲测量),但 band 0 内「等 1 秒」是个大坑。三版尝试的调试叙事(症状→根因→定位→修复→防复发):(1) priority 250 + yield → CPU 密集下被永久饿死,0 行 dump(实测);(2) priority 提高 → 抢占 user code 扭曲测量(逻辑排除,没真跑);(3) band 0 但 sti/hlt → tick 唤醒后 quantum 没耗尽继续选自己,init/child 永远 Ready,gate 卡在第一次 fork、#PF 全程 +0(实测)。正解是 band 0 + yield,与 071 章 sti/hlt 坑同族(都是 Cinux 里 sti/hlt 用错上下文,但机制不同,后文详述)。
5. **§14 file gate 的粒度**:CMake option `CINUX_STATS_KTHREAD`(默认 OFF,[options.cmake:44](../../../cmake/options.cmake#L44))。ON 链 `stats_kthread.cpp`、OFF 链 `stats_kthread_stub.cpp`([CMakeLists.txt:18-24](../../../kernel/mm/CMakeLists.txt#L18))。关键设计——把 gate 放在「整个周期线程」这一层,不是每行 `#ifdef`;PF 计数器和 dump 的 #PF 行是无条件编的(panic 时也能看 #PF),只有「周期采样」这个 ad-hoc profiling 增强被 gate。
6. **dump_memory_stats 的四条正交维度 + PF delta**:`dump_memory_stats` 把「内存压力」拆成 PMM 余量 / slab 页 / PageCache 占用 / 累计 #PF 四条每次同步报全,后来追加第 5 条 ext2 I/O。关键设计是 #PF 用 `static uint64_t last_pf`([diagnostics.cpp:53](../../../kernel/mm/diagnostics.cpp#L53))跨调用算 delta——profiling 靠趋势不靠单点。
7. **诚实的验证边界**:timer_queue 没有独立单测,靠 084 的 `test_poll_finite_timeout_parks_then_returns_zero`([test_poll.cpp:191-204](../../../kernel/test/test_poll.cpp#L191))间接证 arm→park→tick 唤醒→disarm 整链;stats_kthread 没有单测,靠 ON + make run + 串口曲线(CinuxOS dev note 的 31s g++ 曲线是实测,Book 本地未重跑;注意 dev note 那条「126 行 dump」是 ext2 I/O 行还没加进 dump 之前的 4 行/dump 计数,当前源码每次 dump 6 行 [MEM],31s 复现应是 ~186 行);`test_memory_stats.cpp` 测的是 `dump_memory_stats` 这个 FO 组件不是 kthread 本身。这两条边界都要如实标,不拔高。
