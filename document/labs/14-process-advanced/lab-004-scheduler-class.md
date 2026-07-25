---
title: Lab 004 · 调度类与 SIGSTOP/CONT 验证
---

# Lab 004 · 调度类与 SIGSTOP/CONT 验证

> 对应 `document/book/14-process-advanced/004/`。验证档 **A 档**(SIGSTOP/CONT 可见)。验证靠构建 + 测试 + grep + STOP/CONT。

## 目标

确认四件事:

1. `SchedulingClass` 钩子在(`task_tick`/`task_fork`/`task_deadline`),时间片量子内聚到类成员;
2. 优先级感知 RR(`pick_next` 选 priority 最小者);
3. 多调度类实际查询(`pick_next_from`,注册序即优先级);
4. STOP/CONT 真调度(`TaskState::Stopped` + 发送时恢复),run-kernel-test 827→840。

## 步骤

### 1. 构建 + 测试

```bash
git checkout 004_proc_scheduler_class 2>/dev/null || git checkout 004_*
cmake -B build -S . && cmake --build build -j$(nproc) > /tmp/b.log 2>&1; echo "build=$?"
cmake --build build --target run-kernel-test
```

### 2. SchedulingClass + 优先级

```bash
grep -n 'class SchedulingClass\|task_tick\|task_fork\|pick_next_from\|register_class' kernel/proc/scheduler.hpp
grep -rn 'priority' kernel/proc/scheduler.cpp | head
```

**思考**:为什么 priority 小值=高优先级?——对齐 Linux nice(idle 255 垫底)。同优先级并列取 FIFO → 天然 RR;严格优先级可饿死低优先级。

### 3.(插拔真不真)多调度类查询

```bash
grep -n 'pick_next_from\|classes_\[\]\|default_rr_' kernel/proc/scheduler.cpp kernel/proc/scheduler.hpp | head
```

去看 `schedule`/`exit_current` 是不是经 `pick_next_from(classes_, ...)` 遍历 classes_(而不是直调 default_rr_)。**这一弧修的"插拔名存实亡"**:原来 register_class 填了 classes_[] 但没人查,直调 default_rr_;现在三处改走 pick_next_from。

### 4.(A 档)STOP/CONT

```bash
grep -rn 'TaskState::Stopped\|kStop\|kContinue' kernel/proc/signal.cpp kernel/proc/process.hpp
grep -rn 'Stopped.*Ready\|signal_send.*Stopped' kernel/proc/signal.cpp | head
```

**思考**:为什么 SIGCONT/SIGKILL 要在 `signal_send` 发送时恢复 Stopped 目标,不等目标自己投递?——见章节:Stopped 目标永不被调度,自己投递不了。发送时即 Ready+enqueue。

## 验收清单

- [ ] 构建 `build=0`,run-kernel-test ~840。
- [ ] SchedulingClass 钩子在,优先级感知 RR,multi-class 真查询。
- [ ] STOP/CONT 真调度(Stopped 状态 + 发送时恢复 + schedule 排除 Stopped)。
- [ ] 能说清「priority 小=高」「为何 SIGCONT 发送时恢复」「GOTCHA#22 测试用栈 Task」。

## 别做这些

- **别**纯状态机测试用 `TaskBuilder`——消耗全局 tid/slab 跨测污染(GOTCHA#22),用栈 `Task t{}`。
- **别**让 schedule/pick_next 选 Stopped 任务——Stopped 设 Running 必崩;schedule 守卫要排除。
- **别**指望 Stopped 目标自投递 SIGCONT——它不被调度,发送时必须恢复它。
