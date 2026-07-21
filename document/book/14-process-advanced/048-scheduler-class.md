---
title: 048 · 调度类与 SIGSTOP/CONT
---

# 048 · 调度类、优先级、SIGSTOP/CONT:F3 进程弧收官

> 048 收 F3。两件事:验证 **`SchedulingClass` 插拔接口**(策略钩子 + 优先级感知轮询 + 多调度类实际查询),兑现 047 留的 **SIGSTOP/CONT 真调度**(停掉的任务真不被调度、续上真恢复)。A 档:`kill -SIGSTOP` 冻结进程、`SIGCONT` 恢复,是用户可见的。

## 这章咱们要点亮什么

1. **`SchedulingClass` 策略钩子**:`task_tick`(返是否抢占)/`task_fork`(派生子参数)/`task_deadline`(实时预留),默认 no-op。
2. **优先级感知 RoundRobin**:`pick_next` 选 `priority` 最小者(小值=高优先级,Linux 风格,idle=255),并列取 FIFO。
3. **多调度类实际查询**:`pick_next_from` 公开数组原语,注册序即优先级。
4. **STOP/CONT 真调度**:`TaskState::Stopped` + 信号默认动作 kStop/kContinue + 发送时恢复。

## SchedulingClass 插拔

`kernel/proc/scheduler.hpp:42 class SchedulingClass`。策略逻辑从 `Scheduler` 本体抽到可插拔的类:

```cpp
class SchedulingClass {
    void  task_tick(Task* cur);          // 返是否该抢占(时间片到)
    void  task_fork(Task* parent, Task* child);  // 派生子参数(如优先级继承)
    Task* pick_next();                    // 选下一个任务
    void  enqueue(Task* t);
    // task_deadline 实时类预留,默认 0
};
```

加一个新调度算法就是继承 `SchedulingClass` + `Scheduler::register_class(&my_class)`(scheduler.hpp 头部有伪代码示例)。时间片量子从原来的全局 `Scheduler::current_slice_` **内聚到类成员** `quantum_remaining_`(单一事实源),`tick()` 委托给当前任务的 `task_tick`。

## 优先级感知 RoundRobin

默认的 RoundRobin 类 `pick_next` 扫描就绪队列选 `priority` **最小**者(小值=高优先级,对齐 Linux nice,idle 是 255 垫底),并列的取最早入队(FIFO)——同优先级天然轮询,高优先级可饿死低优先级(严格优先级)。这是"优先级 + 轮询"的常见折中。

## 多调度类实际查询

一个隐藏问题被这一弧修了:`register_class` 填 `classes_[]`,但原来的 `schedule`/`exit_current`/`run_first` **直接调 `default_rr_.pick_next()`、从不遍历 `classes_[]`**——插拔名存实亡。这一弧加 `pick_next_from(classes, count)` **公开数组原语**(脱离全局 `default_rr_` 残留态,可单测),三处改走它;注册序即调度类优先级。这下"注册了就会被查到"才真。

## STOP/CONT 真调度(兑现 047 的债)

047 的 SIGSTOP/CONT 只改了信号状态,没真调度效果(没有 Stopped 状态机)。048 补上:

- `TaskState::Stopped` 新状态;
- `signal_exec_default` 对 SIGSTOP 走 `kStop`(Stopped + dequeue + 也许 schedule)、SIGCONT 走 `kContinue`(恢复 Ready + enqueue);
- schedule 守卫排除 Stopped(Stopped 任务永不被 `pick_next` 选中)。

一个不直觉的点:**SIGCONT/SIGKILL 要在 `signal_send` 发送时就恢复 Stopped 目标**,不能等目标自己投递——因为 Stopped 目标永不被调度,它自己投递不了 SIGCONT/SIGKILL。所以发送时即 Ready + enqueue。

## GOTCHA #22:TaskBuilder 消耗全局 tid

批 4 首版用 `TaskBuilder` 建测试 victim,分到 tid 1/2/3;而 `run_signal_tests()` 在 `run_scheduler_tests()` 前跑,导致 `test_build_basic_task` 断言"首任务 tid==1"失败(实际 4+)。修:纯状态机测试用**栈 `Task t{}`**(零 tid/slab/核栈消耗)。

> 通用铁律:**测试别用 `TaskBuilder` 除非真要建可调度 task**;纯逻辑/状态测试用栈对象,避免全局计数器( tid/slab)跨测污染。

## 验证

```bash
grep -n 'class SchedulingClass\|task_tick\|task_fork\|pick_next_from\|register_class' kernel/proc/scheduler.hpp
grep -rn 'TaskState::Stopped\|kStop\|kContinue\|SIGSTOP\|SIGCONT' kernel/proc/
```

构建 + 内核测试(这一弧 run-kernel-test 从 047 的 827 涨到 840):

```bash
cmake --build build -j$(nproc) > /tmp/b.log 2>&1; echo "build=$?"
cmake --build build --target run-kernel-test
```

A 档端到端:发 SIGSTOP 给一个任务 → 它停(STOPPED,不占 CPU)→ 发 SIGCONT → 恢复运行。真 shell 的 Ctrl+Z/bg/fg 要等 TTY 弧(后面)把 SIGSTOP/SIGCONT 接到终端按键。

## 小结与下一站

F3 进程弧全收官(M1-M4):信号、线程、进程组/waitpid、调度类/STOP-CONT。进程/线程这一层的 v1.0.0 升级到位。

下一站 **049** 是横切的 **F-INFRA**(lockdep 锁序图、freestanding 头门禁、NotNull 等基建加固)——插在 F3 和 F4(SMP)之间,给马上要来的多核并发上保险。
