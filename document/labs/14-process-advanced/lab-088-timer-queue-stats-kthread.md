---
title: Lab 088 · timer_queue 与 stats_kthread:定时唤醒 + 周期采样的两条验证路径
---

# Lab 088 · timer_queue 与 stats_kthread:定时唤醒 + 周期采样的两条验证路径

> 088 章把两件内核基础设施的源码过了一遍——`timer_queue` 的固定表 + tick 扫过期 + 锁外唤醒,`stats_kthread` 的 band-0 yield 解法 + §14 file gate。这个 lab 不发答案,只给两条验证路径,你任选其一跑通即算过关。路径一(无需开 gate)用 084 的 `test_poll_finite_timeout_parks_then_returns_zero` 间接证 timer_queue 端到端;路径二(需开 gate)用 ON + make run + 真实 workload 看 stats_kthread 的周期采样曲线。两条都诚实标注边界——timer_queue 没有独立单测,stats_kthread 的曲线数字来自 CinuxOS dev note,Book 本地未重跑。

## 你要确认的事

开始之前,先在 Book 工作树(`/home/charliechen/Cinux`)上核这几样源码都在、行号对得上 088 章:

1. `kernel/proc/timer_queue.hpp` 和 `kernel/proc/timer_queue.cpp` 存在,`kMaxTimers = 32` 在 `.cpp:34`,三接口 `arm`/`disarm`/`tick` 在 `:54` / `:77` / `:90`。
2. `kernel/proc/scheduler.cpp` 的 `Scheduler::tick` 在 `:367`,内部 `timer_queue_tick()` 调用在 `:379`。
3. `kernel/syscall/poll_core.cpp` 的 park 块里 `timer_queue_arm` 在 `:188`、`timer_queue_disarm` 在 `:207`。
4. `kernel/mm/stats_kthread.cpp`(95 行)存在,`stats_thread_entry` 在 `:48`,`start_stats_thread` 在 `:80`,`set_priority(0)` 在 `:86`。
5. `kernel/mm/stats_kthread_stub.cpp`(17 行)存在,空 `start_stats_thread()` 在 `:15`。
6. `kernel/mm/CMakeLists.txt` 的 §14 if/else gate 在 `:18-24`,`cmake/options.cmake:44` 有 `option(CINUX_STATS_KTHREAD ... OFF)`。
7. `kernel/proc/init.cpp:158` 有 `cinux::mm::start_stats_thread();`,周围无 `#ifdef`。

```bash
# 在 Book 工作树根目录跑
ls kernel/proc/timer_queue.hpp kernel/proc/timer_queue.cpp
ls kernel/mm/stats_kthread.cpp kernel/mm/stats_kthread_stub.cpp
grep -n "kMaxTimers\|timer_queue_arm\|timer_queue_disarm\|timer_queue_tick" kernel/proc/timer_queue.cpp
grep -n "timer_queue_tick" kernel/proc/scheduler.cpp
grep -n "CINUX_STATS_KTHREAD" cmake/options.cmake kernel/mm/CMakeLists.txt
grep -n "start_stats_thread" kernel/proc/init.cpp
```

如果上面七样有对不上的,先回头核路径——088 章的链接全是 `kernel/proc/timer_queue.cpp` 这种,别被旧 dev note 误导。

---

## 路径一:timer_queue 端到端(无需开 gate)

这条路径不用动 CMake,直接 build kernel test 跑 084 的那条用例。它的妙处是「不点名 timer_queue 却只可能是 timer_queue 干的」。

### 第一步:读 timer_queue 三接口

先把 `timer_queue.cpp` 通读一遍(就 114 行),重点看三段:

```bash
sed -n '54,75p'  kernel/proc/timer_queue.cpp   # arm:先替换后找空槽,满返 false
sed -n '77,88p'  kernel/proc/timer_queue.cpp   # disarm:清所有匹配槽位
sed -n '90,112p' kernel/proc/timer_queue.cpp   # tick:锁内收集 fired[] 锁外 unblock
```

核三件事:

- `arm` 的两段循环——第一段扫同一 task 的已有 arm 替换 deadline(支持假唤醒后重 park),第二段找空槽,表满返 `false`。
- `disarm` 遍历全表清所有匹配 task 的槽(defensive,理论上一个 task 不会在两槽)。
- `tick` 的「锁内收集 + 锁外唤醒」两段——为什么不在锁内直接 `unblock`?读 `unblock` 干什么(scheduler_block.cpp:48-70),想清楚锁序。

**思考题**(自己答,别翻答案):如果 `tick` 在持有 timer spinlock 时直接调 `unblock`,`unblock` 内部又要拿 run-queue 锁——别处若有「先拿 run-queue 锁再拿 timer 锁」的路径,会发生什么?(提示:经典 AB-BA。Cinux 调度器实际没这种反向路径,但纪律要立住——锁序严格 timer_lock → run-queue lock 绝不反着。)

### 第二步:读 Scheduler::tick 怎么驱动 timer_queue

`timer_queue_tick` 是被 PIT 驱动的 `Scheduler::tick` 每次中断调的。读一遍:

```bash
sed -n '367,379p' kernel/proc/scheduler.cpp
```

核两件事:

- `timer_queue_tick()` 在 `:379`,跑在 tick 最前——先唤醒过期 task,让它们在紧接着的 pick 里可被选中。
- 注释 `:375-378` 写明「Cheap no-op when nothing is armed (one locked scan of a small table)」——空表是一次关 IRQ + 扫 32 个 `armed=false` 的廉价 no-op。

**思考题**:`Scheduler::tick` 跑在 IRQ 上下文(IF=0)。`timer_queue_tick` 内部又 `g_lock.irq_guard()` 关一次 IRQ——这会不会双重关 IRQ 出问题?(提示:`irq_guard` 是 RAII,构造时关、析构时恢复到进入时的状态——IRQ 上下文里关 IRQ 是幂等的,恢复时仍是关着,没问题。)

### 第三步:跑 test_poll_finite_timeout,解释「为什么 30ms 后真能返回」

这是路径一的核心。读一遍用例:

```bash
sed -n '191,204p' kernel/test/test_poll.cpp
```

这条用例 poll 一根空 pipe、超时 30ms、期望 `r==0` 且 `rv==0`。它没点名 `timer_queue`,但「30ms 后真能返回」**只可能是** timer_queue 在 tick 里唤醒了被 park 的任务——这是 timer_queue 端到端的活证据。

你要能复述整条链(这是过关标准):

1. `poll_one_timeout` 进 `do_poll_core` 的 park 块(`poll_core.cpp:180`)。
2. `InterruptGuard`(`:184`)关 IRQ。
3. `prepare_to_wait`(`:185`)翻 state → Blocked。
4. `register_all`(`:186`)挂 read 队列。
5. `timer_queue_arm(self, deadline)`(`:188`)挂 30ms deadline。
6. `schedule_blocked`(`:204`)真切走。
7. PIT 驱动的 `Scheduler::tick`(`scheduler.cpp:367` 定义)每次中断在 `:379` 调 `timer_queue_tick`,30ms 后扫到 deadline 过期,锁内收 `fired[]`、锁外 `Scheduler::unblock`。
8. poller 醒来,`detach_all`(`:205`)撤销 fd 注册、`timer_queue_disarm`(`:207`)清自己的槽位。
9. 回 for 顶 Pass 1 重扫,空 pipe 没就绪、deadline 已过,返回 0、revents=0。

跑这条测试(具体命令看 `kernel/test/` 的入口,一般是 `make test_poll` 或进 kernel/test 目录跑):

```bash
# 跑 poll 测试,确认 test_poll_finite_timeout_parks_then_returns_zero 过
# 看一眼 kernel/test/Makefile 或 README 找入口
```

如果这条过了——timer_queue 端到端验过(arm→park→tick 唤醒→disarm 整链闭合)。

### 第四步(可选加压):把超时从 30ms 改成 5ms

想验证 tick 粒度够不够细,自己改一下超时:

```bash
# 临时改:把 test_poll.cpp:199 的 30 改成 5
# poll_one_timeout(rfd, kPollIn, 5, rv);
```

预期 `r==0` 仍然成立——PIT tick 频率(~100Hz,10ms 周期)足够捕获 5ms 超时(可能略晚几 ms 返回,但不会丢)。这反证 tick 粒度对短超时够用。

**验完务必 `git checkout` 还原**——这是临时改动,不进 main。

```bash
git checkout kernel/test/test_poll.cpp
```

### 路径一的过关标准

- [ ] 能复述 timer_queue 三接口(arm/disarm/tick)各自干什么。
- [ ] 能解释 `tick` 为什么锁内收集、锁外唤醒(锁序纪律)。
- [ ] 能复述 `test_poll_finite_timeout` 那条 9 步链。
- [ ] 测试过了(`r==0, rv==0`)。
- [ ] 改 5ms 仍过(可选)。

---

## 路径二:stats_kthread 真周期采样(需开 gate)

这条路径要开 `CINUX_STATS_KTHREAD`,跑真实 workload,看串口曲线。比路径一折腾,但能亲眼看到 stats_kthread 真在干活。

### 第一步:确认 §14 gate 接线

先核 CMake 选链逻辑:

```bash
sed -n '13,24p' kernel/mm/CMakeLists.txt   # if/else gate
sed -n '44p'     cmake/options.cmake        # option 默认 OFF
cat kernel/mm/stats_kthread_stub.cpp        # OFF 时的空函数
grep -n "start_stats_thread" kernel/proc/init.cpp   # 无条件调用,无 #ifdef
```

核四件事:

- `option(CINUX_STATS_KTHREAD ... OFF)` 默认 OFF。
- ON 链 `stats_kthread.cpp`、OFF 链 `stats_kthread_stub.cpp`。
- stub 是个空 `void start_stats_thread() {}`,签名和真实现一模一样。
- `init.cpp:158` 无条件调 `start_stats_thread()`——**没有任何 `#ifdef` 包裹**。

**思考题**:为什么 `init.cpp` 不用 `#ifdef CINUX_STATS_KTHREAD` 包住这行调用?(提示:链接器替它选 TU——OFF 时这行调的是 stub 的空函数,ON 时调的是真实现。源码零 `#ifdef`,读起来和普通函数调用一样干净。这是 file gate 相对 `#ifdef` 的核心优势。)

### 第二步:读 stats_thread_entry,搞清 yield 解法

读 `stats_kthread.cpp` 全文(95 行),重点看主循环:

```bash
sed -n '48,78p' kernel/mm/stats_kthread.cpp   # stats_thread_entry
sed -n '12,30p' kernel/mm/stats_kthread.cpp   # 头注释:三版尝试
```

核两件事:

- 主循环 `while(true)` 开头是 `Scheduler::yield()`(`:59`),不是 `sti/hlt`、不是低优先级。
- 每轮 yield 醒来查 HPET deadline,到了才 dump,不到回去再 yield——「醒得很勤(约 100Hz)但干活很稀(1Hz dump)」。

读头注释 `:12-24`,搞清三版尝试(两版实测失败 + 一版逻辑排除):

- priority 250 → CPU 密集下饿死,0 行 dump;
- priority 提高 → 抢占 user code 扭曲测量;
- band 0 + sti/hlt → tick 唤醒后 quantum 没耗尽继续选自己,init/child 永远 Ready,gate 卡在第一次 fork。

**思考题**:为什么 `yield` 能同时满足「不饿死编译」「不扭曲测量」「不垄断 CPU」三个约束,而 `sti/hlt` 不能?(提示:`yield` 是协作式让出——让出 CPU 给下一个 band-0 task,它干活、你睡;PIT tick 切回你,你查 deadline。`sti/hlt` 把 CPU 交给中断,但带优先级的调度器在中断返回时不一定切走,导致 halt 的线程反复被选中——跟 071 章那个 sti/hlt 坑同族,但机制不同:071 是 syscall 里 sti 撞陷阱帧出 #DF,这里是 band-0 hlt 让调度器反复选自己饿死同级任务。)

**反直觉点**:stats_kthread 为什么不用 timer_queue park 自己挂 1 秒 deadline?读 timer_queue 的 park 语义(`prepare_to_wait` 翻 Blocked),想清楚 park 的 task 在 band 0 里会不会撞上「谁来叫醒它」的问题。

### 第三步:开 gate,make run,跑真实 workload

```bash
# 配置开 gate
cmake -DCINUX_STATS_KTHREAD=ON <其他 Cinux 标配参数> build_dir
# 或直接改 cmake/options.cmake 把 OFF 改 ON,再 cmake

# 构建 + 跑
make
make run
```

进 QEMU shell 后,跑一个真实 workload。典型 buildroot rootfs 里有 g++:

```sh
# 在 QEMU shell 里
echo '#include <iostream>
int main() { std::cout << "hello" << std::endl; return 0; }' > hello.cpp
g++ hello.cpp -o hello
./hello
```

这就是 CinuxOS dev note 要诊断的「g++ 编译卡顿」场景。编译几十秒,串口应该每隔 ~1 秒出一组 `[MEM]` 六行(t=/PMM/Slab/PageCache/#PF/I/O)。

### 第四步:读曲线,识别「爆发期」和「基线期」

串口日志每隔 ~1 秒该出现六行(t= 时间戳 + 五条数据行),格式(参考 `diagnostics.cpp:36-66`):

```
[MEM] === t=N.NNN s ===
[MEM] PMM:       <free> / <total> pages free (<free>*4 KiB free of <total>*4 KiB)
[MEM] Slab:      <slab_pages> slab pages mapped
[MEM] PageCache: <cached> cached (<hits> hits / <misses> misses)
[MEM] #PF:       <total> total (+<delta> since last dump)
[MEM] I/O:       <reads> reads (+<delta>), <KiB> KiB, <ms> ms (+<ms> ms)
```

**读曲线检查清单**(每次 dump 该出现哪六行、t= 行该不该递增、PF delta 该不该随 workload 波动):

- [ ] `t=N.NNN s` 行每秒递增(HPET 时间戳,真实时间轴)。
- [ ] PMM free 在编译期大致稳定(8GB 充裕的话,~2.08M 页量级,波动几千页正常)。
- [ ] PageCache cached 在编译期单调增长(cc1/cc1plus 加载文件页),编译完趋于稳定。
- [ ] **#PF delta 该随 workload 波动**——编译期(sec 8/13/20 这种 cc1 加载大库的时刻)delta 飙到几千甚至 +18272,编译完 delta 回到个位数。这是 `static last_pf` 算 delta 的核心价值——单调总量 31117 没意义,但 sec 20 的 +18272 一眼定位到 cc1plus 加载 libstdc++ 的 demand paging 爆发。
- [ ] I/O reads/bytes/ms delta 也该在编译期飙高(ext2 读 cc1 二进制 + libstdc++.so 等)。

对照 CinuxOS dev note(`/home/charliechen/CinuxOS/document/notes/2026-07-05-perf-b1-stats-profiling.md`)的 31s 曲线片段:

```
sec  PMM_free   Cache   #PF_tot  +delta    阶段
0    2088396      3        31     +31       <- boot 早期基线
...
20   2055062    1522     22882   +18272     <- cc1plus + libstdc++ 加载(峰值)
...
30   2086187    1767     31117     +7       <- g++ 完成,趋于平静
```

**诊断结论**(dev note 的):PMM 始终 ~2.08M 页(8GB 充裕)、Cache 才涨到 1767 页(~7MB)、PF avg 1004/s 爆发集中在 sec 8/13/20(对应 cc1/cc1plus 加载大库),**内存子系统不是瓶颈**——卡顿要往 QEMU TCG 翻译开销 / disk I/O / 热 syscall 找。

### 诚实标注

路径二的曲线数字(PMM ~2.08M 页 / Cache 1767 页 / +18272 PF / 31s)**来自 CinuxOS dev note**(`/home/charliechen/CinuxOS/document/notes/2026-07-05-perf-b1-stats-profiling.md`),**Cinux-Book 本地未重跑**。注意 dev note 写的「126 行 dump (31 s × 4 行)」是 B2.5 加 ext2 I/O 行之前的计数(那时每次 dump 4 行 [MEM]);当前源码每次 dump 6 行 [MEM](t=/PMM/Slab/PageCache/#PF/I/O),所以同样 31s 复现会是 ~186 行——别看到 ~186 行就以为哪里错了,这是源码演进的正常结果。你若在本机 ON + make run 复现,数字会因 rootfs 配置(装了哪些共享库)、QEMU 配置(内存大小、CPU 数、是否 KVM)、workload(编什么文件)略有差异——但**趋势该一致**:编译期 PF delta 飙高、Cache 单调增长、PMM 余量稳定。读到这个趋势就算过关。

### 路径二的过关标准

- [ ] 能复述三版尝试(priority 250 饿死[实测] / 提高 扭曲[逻辑排除] / sti-hlt 卡死[实测])。
- [ ] 能解释 `yield` 为什么是正解(三约束同时满足)。
- [ ] ON 配置真编过、make run 真启动到 init。
- [ ] 串口每隔 ~1 秒真出一组 `[MEM]` 六行(t= 递增)。
- [ ] 能读出「爆发期」(PF delta 飙高)和「基线期」(PF delta 个位数)的区别。
- [ ] 能解释为什么 dump 用 `static last_pf` 算 delta 而不是单调总量(profiling 靠趋势不靠单点)。

---

## 收尾:把 088 章的声明逐条对上

跑完任一路径,回头逐条核 088 章的「咱们要点亮什么」七条,每条都能在源码或测试里找到证据:

| 088 章声明 | 证据位置 |
|---|---|
| timer_queue 固定表 + tick 扫过期 | `timer_queue.cpp:34` kMaxTimers=32、`:90-112` tick 锁内收集锁外 unblock |
| 锁外唤醒的锁序纪律 | `timer_queue.cpp:11-14` 头注释、`:106` 注释「Wake outside the timer lock」、`scheduler_block.cpp:48-70` unblock 拿 run-queue 锁 |
| stats_kthread 每秒 dump_memory_stats | `stats_kthread.cpp:46` kStatsIntervalNs=1e9、`:74-76` due 时调 dump |
| band 0 + yield 三版尝试正解 | `stats_kthread.cpp:12-24` 头注释、`:54-59` yield 主循环、`:86` set_priority(0) |
| §14 file gate 源码零 #ifdef | `options.cmake:44` option、`CMakeLists.txt:18-24` if/else、`init.cpp:158` 无条件调用、`stats_kthread_stub.cpp:15` 空函数 |
| dump 四条正交维度 + PF delta | `diagnostics.cpp:36/40/43/45/54/66` 六行(1 timestamp + 5 数据行)、`:53` static last_pf |
| 诚实验证边界 | `test_poll.cpp:191-204` 间接证 timer、`test_memory_stats.cpp:21` 只测 dump 非 kthread、CinuxOS dev note 曲线 |

七条全对上,088 章的「教程即验证」才算闭环。如果某条对不上(比如 grep 不到 `timer_queue_tick` 在 `scheduler.cpp:379`、或者 `init.cpp:158` 那行被 `#ifdef` 包了),回头读 088 章对应小节,看是源码演进了还是章节写错了——教程是 tag-bound 的,以当前工作树的源码真值为准。

### 进一步的折腾(可选)

- 把 `kMaxTimers`(`timer_queue.cpp:34`)从 32 改成 1,跑 084 的 `test_poll_finite_timeout`——仍能过(单次 arm 不会撞表满)。再改 poll 测试同时 poll 多个 fd 各带 timeout(若有的话),第二个 arm 会返 `false`,poller 降级成无 timeout park。这能让你亲手触发「表满降级」的边界。**验完还原。**
- 读 `Scheduler::unblock`(`scheduler_block.cpp:48-70`)的幂等注释(`:53`「Idempotent (F4-M4 prepare-to-wait)」),思考「如果 unblock 不是幂等,timer 和 fd 双唤醒源会怎样」——fd 那侧叫了一次,timer 这侧又叫一次,后者若 enqueue 就 double-add 进运行队列。这条幂等是 timer_queue 锁外唤醒能安全跑的前提。
- 读 `dump_memory_stats` 的 `static last_pf`(`diagnostics.cpp:53`),思考「如果哪天加了第二个并发调用者(比如某个 syscall 也调 dump),这个 static 会怎样」——会 race。这就是注释里「no concurrent callers in practice (panic once + the single stats thread)」这条不变量的分量。如果真要加第二个调用者,得把这个 static 改成 per-caller state 或加锁。
- grep `sys_nanosleep` 在 `kernel/syscall/`——确认它仍是 yield 自旋(`sys_nanosleep.cpp:50-55`),没迁 timer_queue。这是 088 章标的「nanosleep 这条现在其实没生效」。如果你想给 nanosleep 接 timer_queue,这是个 DEBT 入口——但那是单独的活,不混进 lab。
- 对照 `usb_stub.cpp`(USB 编译关时的空 init)和 `tlb_drain_stub`(TLB drain kthread 关时的空 spawn)——它们和 `stats_kthread_stub.cpp` 是同一套 file gate 模式。grep `stub.cpp` 在整棵树,数数 Cinux 有多少个这种「可选增强收进 file gate」的先例。

这些折腾不要求做完,挑一个顺眼的深挖。088 章主线是 timer_queue 内部 + stats_kthread 调度难点 + §14 gate,这几个延伸是「顺手吃下的并发/调试入口」。
