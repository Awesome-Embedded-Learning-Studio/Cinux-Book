---
title: 03 · stats_kthread:周期采样的常驻 kthread + §14 file gate
---

# stats_kthread:周期采样的常驻 kthread + §14 file gate

timer_queue 是给「别人」用的——084 poll 用它挂 deadline。stats_kthread 不一样,它**自己**是个常驻内核线程,目的是给「g++ hello.cpp 卡顿」这类 workload 做 ad-hoc 性能剖析:每 ~1 秒调一次 `dump_memory_stats()`,把 PMM free 页数 / slab 页数 / PageCache 占用 / 累计 #PF / ext2 I/O 打到串口,形成一条曲线。读趋势(哪一秒 cache 暴涨、哪一秒 #PF 飙到 +18272)就能定位根因,而不是猜——这是 Cinux memory 那条「debuggability over perf」铁律的落地。

采样体 `stats_thread_entry`([stats_kthread.cpp:48](../../../kernel/mm/stats_kthread.cpp#L48)):

```cpp
// kernel/mm/stats_kthread.cpp:48-78
void stats_thread_entry() {
    kprintf("[MEM] stats thread entry running\n");
    const bool have_hpet     = cinux::drivers::g_hpet.available();
    uint64_t   next_deadline = cinux::drivers::g_hpet.monotonic_ns() + kStatsIntervalNs;
    int        ticks_since_dump = 0;

    while (true) {
        // yield() hands the CPU to the next band-0 task (init/cc1/g++); the PIT
        // tick preempts it back to us, so we wake ~once per tick (~10 ms) --
        // far below the 1 s dump interval.  See file header for why this is NOT
        // sti/hlt and NOT a lower priority.
        cinux::proc::Scheduler::yield();

        bool due = false;
        if (have_hpet) {
            due = cinux::drivers::g_hpet.monotonic_ns() >= next_deadline;
            if (due) {
                next_deadline += kStatsIntervalNs;
            }
        } else {
            // No HPET: fall back to a tick count (~100 Hz PIT => ~1 s).
            due = (++ticks_since_dump >= 100);
            if (due) {
                ticks_since_dump = 0;
            }
        }
        if (due) {
            dump_memory_stats();
        }
    }
}
```

整个 entry 是个 `while(true)`:每轮先 `Scheduler::yield()` 让出 CPU,醒来后查 HPET `monotonic_ns()` 是否过了 `next_deadline`(初始 = 启动时刻 + 1e9 ns);到了就 `dump_memory_stats()` 并把 deadline 推后 1 秒;没到就回去再 yield。HPET 不可用时退化成数 tick(`++ticks_since_dump >= 100`,~100 Hz PIT 凑 ~1 秒)。

计时基准用 HPET free-running counter,不依赖 PIT IRQ 频率——ns 级粒度。1 秒间隔是粗放有意的:编译几十秒,1 Hz 够画曲线又不刷爆串口(注释 [stats_kthread.cpp:44-46](../../../kernel/mm/stats_kthread.cpp#L44) 写明「Coarse on purpose: a compile takes tens of seconds, so a ~1 Hz sample yields plenty of points without flooding the serial log」)。

为什么这里用 yield 而不是用 timer_queue?这看起来反直觉——既然有了 timer_queue 这么漂亮的定时唤醒原语,为什么 stats_kthread 不 park 自己挂个 1 秒 deadline?下一节专门讲。

## spawn 路径

`start_stats_thread`([stats_kthread.cpp:80](../../../kernel/mm/stats_kthread.cpp#L80)):

```cpp
// kernel/mm/stats_kthread.cpp:80-93
void start_stats_thread() {
    // priority 0 = the TaskBuilder default, same band as kernel_init and the
    // fork'd user processes.  See file header: lower starves, higher skews.
    auto* t = cinux::proc::TaskBuilder()
                  .set_entry(stats_thread_entry)
                  .set_name("mm_stats")
                  .set_priority(0)
                  .build();
    if (t != nullptr) {
        cinux::proc::Scheduler::add_task(t);
        kprintf("[MEM] periodic stats thread started (%llu ms interval)\n",
                static_cast<unsigned long long>(kStatsIntervalNs / 1'000'000));
    }
}
```

`TaskBuilder().set_entry(...).set_name("mm_stats").set_priority(0).build()`——`build()` 返 `nullptr` 时静默不 `add_task`、不 panic(内存紧张建不出 task 就算了,采样是 ad-hoc 增强不是关键路径)。`set_priority(0)` 进 band 0——下一节讲为什么必须 0。

`init.cpp` 在 `launch_userspace` 之前无条件调它([init.cpp:155-158](../../../kernel/proc/init.cpp#L155)):

```cpp
// kernel/proc/init.cpp:155-158
// B1 gcc-stutter profiling: spawn the periodic memory-stats kthread.  No-op
// when CINUX_STATS_KTHREAD=OFF (stub); prints a 1 Hz PMM/slab/PageCache/#PF
// curve to the serial log when ON, for narrowing gcc/g++ compile-stutter.
cinux::mm::start_stats_thread();
```

注意这行**没有任何 `#ifdef` 包裹**——OFF 时这行调的是 stub 的空函数,零成本零回归。这就是 §14 file gate 的精髓。

## §14 file gate:CMake 选链,源码零 #ifdef

CMake option 在 [options.cmake:44](../../../cmake/options.cmake#L44):

```cmake
# cmake/options.cmake:44
option(CINUX_STATS_KTHREAD "Spawn periodic 1 Hz memory-stats kthread for ad-hoc profiling" OFF)
```

默认 OFF。选链逻辑在 [CMakeLists.txt:13-24](../../../kernel/mm/CMakeLists.txt#L13):

```cmake
# kernel/mm/CMakeLists.txt:13-24
# B1 gcc-stutter profiling: §14 file gate. ON links the real periodic stats
# kthread (stats_kthread.cpp); OFF links the empty stub (stats_kthread_stub.cpp)
# so init.cpp's start_stats_thread() call resolves either way -- no #ifdef in
# source. The PF counter + dump_memory_stats PF line are unconditional (built
# into page_fault.cpp / diagnostics.cpp); only the periodic thread is gated.
if(CINUX_STATS_KTHREAD)
    target_sources(big_kernel_common PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/stats_kthread.cpp)
else()
    target_sources(big_kernel_common PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/stats_kthread_stub.cpp)
endif()
```

ON 链 `stats_kthread.cpp`(真实现),OFF 链 `stats_kthread_stub.cpp`([stats_kthread_stub.cpp:15](../../../kernel/mm/stats_kthread_stub.cpp#L15)):

```cpp
// kernel/mm/stats_kthread_stub.cpp:13-17
namespace cinux::mm {

void start_stats_thread() {}

}  // namespace cinux::mm
```

空函数,签名一模一样。链接器替 `init.cpp` 选 TU——`init.cpp` 完全不用关心开关,读起来和普通函数调用一样干净。这套模式和 `usb_stub.cpp`(USB 编译关时的空 init)/ `tlb_drain_stub`(TLB drain kthread 关时的空 spawn)是同一套——Cinux 把「可选增强」收进 file gate 的标准姿势。

更细的一点:`PF` 计数器([page_fault.cpp:62](../../../kernel/arch/x86_64/page_fault.cpp#L62))和 `dump_memory_stats` 的 #PF 行([diagnostics.cpp:54](../../../kernel/mm/diagnostics.cpp#L54))是**无条件编的**——panic dump 也能看 #PF——只有「周期采样线程」被 gate。注释原文([CMakeLists.txt:16-17](../../../kernel/mm/CMakeLists.txt#L16)):「The PF counter + dump_memory_stats PF line are unconditional; only the periodic thread is gated」。理由很实在:panic 时调一次 `dump_memory_stats` 看 #PF 总数是 debuggability 基线,不该被一个 ad-hoc profiling 的开关牵连;而周期采样是「跑 workload 时多打一堆串口」的增强,默认关掉避免污染日常日志。
