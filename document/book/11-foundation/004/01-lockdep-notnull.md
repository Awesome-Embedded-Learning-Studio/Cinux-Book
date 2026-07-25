---
title: 01 · lockdep 与 NotNull:把纪律写进类型与构建
---

# lockdep 与 NotNull:把纪律写进类型与构建

> 马上要做 SMP 多核——并发是死锁、竞态的高发区。单核时侥幸不出事的代码,多核会爆。这一章在多核到来之前,先给两样东西上保险:一是**锁纪律的自动检查**(lockdep),把"持着自旋锁去调度"这类经典死锁源,从"难复现的隐患"变成"开发期当场炸";二是把"这个指针不能为空"这种**注释契约写进类型**(NotNull)。顺带还有一批"看不见但扛事"的构建加固。

## lockdep:持着锁去调度?当场抓

持着自旋锁去 `schedule()` 是内核里一类经典 bug——别的任务要拿同一把锁就死锁,中断路径撞上就竞态。单核时偶尔侥幸(中断正好没来),多核必爆。lockdep 给它一个**开发期的自动检查**:

```cmake
# kernel/CMakeLists.txt —— 可选打开(有运行时开销,是开发期检查)
if(CINUX_LOCKDEP)
    target_compile_definitions(big_kernel_common PUBLIC CINUX_LOCKDEP)
endif()
```

打开时,每个自旋锁的获取/释放维护一个全局"持锁深度",`schedule()` 入口断言它是 0:

```cpp
// kernel/proc/scheduler.cpp:402(lockdep 块)
#ifdef CINUX_LOCKDEP
    if (uint32_t d = lockdep_held_depth(); d > 0) {
        // 持着锁跨 schedule —— 死锁/竞态根源,直接 panic 暴露
    }
#endif
```

> 注意这里的深度查询是**函数** `lockdep_held_depth()`(per-CPU),不是 Part1 的全局变量 `g_lockdep_held_depth`。Part1 的全局计数器是 SMP-unsafe 的,本章这版换成了 per-CPU 版本——旧名现在只活在 `lockdep.hpp` 的一句历史注释里("Replaces the Part1 global g_lockdep_held_depth")。

为什么**默认关**?因为它有运行时开销,而且是**开发期检查**——开发、CI 打开它跑测试,持锁调度的代码当场炸;生产构建关掉,零开销。这和 Linux 的 `CONFIG_LOCKDEP` 一个路子。它是多核并发的"保险":把"偶发的 Heisenbug"变成"开发期必炸",省下无数难复现的调试时间。

## NotNull:把"不能为空"写进类型

`lib::NotNull<T*>` 是个"不能为空"的指针类型。这一章把调度器的几个关键接口参数从 `Task*` 换成 `NotNull<Task*>`:

```cpp
// kernel/proc/scheduler.cpp
void Scheduler::add_task(lib::NotNull<Task*> task, bool wake_ap = true);
void Scheduler::remove_task(lib::NotNull<Task*> task);
void Scheduler::run_first(lib::NotNull<Task*> boot_task);
```

(`add_task` 还有个默认参数 `wake_ap = true`,决定加进来时是否给目标 CPU 发 IPI 唤醒——和本章主题无关,这里点一句省得对不上源码。)

意义:把"`add_task` 的参数不能传空"这条**注释契约**写进**类型**——传空编不过,不用靠运行时检查或文档。这和 036 的 ErrorOr(用类型表达错误)一脉相承——能用类型挡住的 bug,就别留给运行时。
