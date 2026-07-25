---
title: 01 · 让调度策略可插拔,让 SIGSTOP/CONT 真起作用
---

# 让调度策略可插拔,让 SIGSTOP/CONT 真起作用

> 两个目标。一是把调度器从"写死的一种轮询"改成**可插拔的调度策略**(调度类),并加上**优先级**——高优先级先跑、同优先级轮询。二是兑现 047 留的债:`SIGSTOP`/`SIGCONT` 之前只改了信号状态,没有真调度效果(停掉的任务其实还在被调度)。这一章给它加上"停止"状态,让被 STOP 的任务**真不被调度**,CONT 时真恢复。

## 调度类:策略可插拔

把调度逻辑从 `Scheduler` 本体抽到一个可插拔的**调度类**(`kernel/proc/scheduler.hpp`):

```cpp
class SchedulingClass {
    void  task_tick(Task* cur);                  // 时钟到了,返是否该抢占
    void  task_fork(Task* parent, Task* child);   // 派生子的调度参数
    Task* pick_next();                            // 选下一个跑谁
    void  enqueue(Task* t);
};
```

想加一种新调度算法(比如严格优先级、实时轮转),就继承 `SchedulingClass` + 注册进去,不用动 `Scheduler` 本体。时间片配额也从原来的"调度器全局变量"**收进调度类**自己管(单一事实源),时钟中断委托给当前任务的 `task_tick` 决定要不要抢占。

## 优先级感知的轮询

默认的调度类,`pick_next` 不再是无脑取队头,而是扫一遍就绪队列、选**优先级数字最小**的(数字小 = 优先级高,对齐 Linux 的 nice;idle 任务是 255,垫底)。并列的取最早入队的——于是**同优先级天然轮询,高优先级可以饿死低优先级**(严格优先级)。这是"优先级 + 轮询"的常见折中。

## 多调度类:真查,不是摆设

这里修了一个隐藏问题:本来"注册调度类"填进一个数组,可调度器实际选任务时**直调默认那个、从不遍历数组**——插拔名存实亡。这一章加了个"按数组选"的原语,让注册的调度类真被查到;注册顺序就是调度类的优先级。这下"注册了就会被用到"才成立。

## SIGSTOP/CONT:真停真续

047 的 SIGSTOP/SIGCONT 只改了信号侧状态。这一章给任务加一个**停止状态**,让它真起调度效果:

- 收到 SIGSTOP → 任务进**停止态**、从就绪队列摘掉、可能触发调度;
- 收到 SIGCONT → 恢复**就绪态**、放回队列;
- 调度器的 `pick_next` **排除停止态**的任务(它们永不被选中)。

一个不直觉的点:**SIGCONT(以及 SIGKILL)发给一个停止态的任务时,要在 `signal_send` 发送的那一刻就把它恢复**,不能等它自己投递——因为停止态的任务不被调度,它自己根本没机会投递 SIGCONT 给自己。所以发送时就把它唤醒、放回队列。

> 一个测试的坑:`TaskBuilder` 会消耗一个全局的 tid 计数器。纯状态机的测试(不真要一个可调度的任务)别用 `TaskBuilder`,用栈上的 `Task t{}`——否则 tid 计数器跨测试污染,后面断言"第一个任务 tid==1"就挂了。

## 验证

```bash
grep -n 'class SchedulingClass\|task_tick\|task_fork\|pick_next_from\|register_class' kernel/proc/scheduler.hpp
grep -rn 'TaskState::Stopped\|kStop\|kContinue' kernel/proc/
```

构建:

```bash
cmake -B build -S . && cmake --build build -j$(nproc) > /tmp/b.log 2>&1; echo "build=$?"
```

端到端:给一个任务发 SIGSTOP → 它停(停止态,不占 CPU)→ 发 SIGCONT → 恢复运行。真 shell 的 Ctrl+Z / bg / fg 要等后面的终端弧把 SIGSTOP/SIGCONT 接到按键和会话概念上;但"任务能被真停、真续"这个内核能力,这一章到位了。
