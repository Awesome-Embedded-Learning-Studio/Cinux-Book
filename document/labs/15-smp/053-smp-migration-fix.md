---
title: Lab 053 · SMP 迁移竞态修复验证
---

# Lab 053 · SMP 迁移竞态修复验证

> 对应 `document/book/15-smp/053-smp-migration-fix.md`。验证档 **B 档**(机制/重构,靠内核测试不回归 + grep)。本章修的是 052 留的债:任务跨核迁移时,旧核存 ctx、新核取同一份 ctx 并发写花,导致 `-smp 2` panic。修法对齐 Linux `task_struct->on_cpu`。

## 目标

确认五件事:

1. `Task` 有 `on_cpu` 字段(`-1`=已存/未运行,`cpu_id`=运行中),且有 `static_assert` 钉死偏移;
2. `context_switch.S` 存完 from 的 ctx 后立刻设 `from->on_cpu = -1`;
3. `pick_next` 跳过 `on_cpu != -1` 且不是本核的任务(本核不跳);
4. `schedule` 切入 next 前先 `on_cpu = 本核` 认领;`TaskBuilder::build` 初始化新任务 `on_cpu = -1`;
5. 单核 `run-kernel-test` 不回归(931 全绿)。

## 步骤

### 1. on_cpu 字段 + 偏移锁定

```bash
grep -n 'on_cpu' kernel/proc/process.hpp
```

去看 `on_cpu` 字段定义(约 `process.hpp:160`)和 `static_assert(offsetof(Task, on_cpu) == sizeof(CpuContext), ...)`(约 `:300`)。这个断言把 `on_cpu` 钉死在 `sizeof(CpuContext)` 偏移上,这样 `context_switch.S` 里 `96(%rdi)` 那条写才永远对得上——布局一变编译期就炸。

### 2. 存完才放行(汇编)

```bash
grep -n 'on_cpu\|movl.*-1.*96' kernel/arch/x86_64/context_switch.S
```

应看到存完 from 的 callee regs/rsp/rip/fs_base 之后,一条 `movl $-1, 96(%rdi)`(`context_switch.S:78` 附近)把 `from->on_cpu` 置 -1。这一步必须在汇编里做——`context_switch` 的「返回」是任务被切回来时,不是「prev 存完了」,所以 C 层做不到「存完即标记」。

### 3. pick_next 跳过正在被别核存的任务

```bash
grep -n 'on_cpu' kernel/proc/roundrobin.cpp
```

`pick_next` 应跳过 `on_cpu != -1 && on_cpu != 本核` 的任务(约 `roundrobin.cpp:92`)。注意本核的**不跳**——保住单核 `yield` 时 `next==prev` 直接返回的语义。

> **思考**:为什么是「跳过」不是「自旋等 `on_cpu` 变 -1」?——见章节:两个核互等对方存完会环形死锁(cpu0 等 cpu1 的任务、cpu1 等 cpu0 的任务,都卡在 pick_next 推进不了自己的 context_switch)。跳过(挑别的或 idle)无死锁,任务留队列等下一轮。这是「可调试优先于性能」的取舍。

### 4. 取之前先认领 + 新任务初始化

```bash
grep -n 'on_cpu' kernel/proc/scheduler.cpp kernel/proc/task_builder.cpp
```

应看到:`schedule`/`run_first` 切入 next 前用 `__atomic_store_n(&next->on_cpu, cpu_id, __ATOMIC_RELEASE)` 认领(`scheduler.cpp:281`、`:415`);`TaskBuilder::build` 给新任务 `task->on_cpu = -1`(它从没跑过,没人存它的 ctx)+ `quantum_remaining = DEFAULT_TIME_SLICE`。

### 5. 单核不回归

```bash
cmake --build build --target big_kernel_test -j$(nproc) > /tmp/b.log 2>&1; echo "build=$?"
cmake --build build --target run-kernel-test 2>&1 | grep -E 'Tests:|ALL TESTS'
```

> **别** `cmake --build build`(ALL):host 单测既有债会让 ALL 失败,跟本章无关。用 `big_kernel_test` + `run-kernel-test`。

应 `build=0`、`Tests: 931 passed, 0 failed`。`on_cpu` 纪律在单核是 no-op(本核任务不跳过),所以单核行为不变——这是它不破坏单核的关键。

> 双核 `-smp 2` 的 panic 归零验证需要带 SMP 的运行 target(如 `run-kernel-test-smp` 或 `make run -smp 2`),且本机 QEMU 的 AHCI 时序竞态是环境相关的——见章节「诚实边界」。本章修的是调度竞态**根因**,单核测试不回归是它能合入的硬门禁。

## 验收清单

- [ ] `Task` 有 `on_cpu` 字段;`static_assert` 钉死偏移 = `sizeof(CpuContext)`。
- [ ] `context_switch.S` 存完 from 后 `movl $-1, 96(%rdi)`(标记可取)。
- [ ] `pick_next` 跳过 `on_cpu != -1 && != 本核`(本核不跳,保单核 yield 语义)。
- [ ] `schedule` 认领 `next->on_cpu = 本核`(release);`TaskBuilder::build` 初始化 `on_cpu = -1`。
- [ ] `big_kernel_test` `build=0` + `run-kernel-test` 931 全绿(单核不回归)。

## 别做这些

- **别**用「自旋等 `on_cpu` 变 -1」代替「跳过」——两核互等会环形死锁。
- **别**把「标记 prev 存完」放到 C 层——`context_switch` 的返回语义是「被切回来」,不是「prev 存完」;只能在汇编里存完 from 后立即写。
- **别**在 `pick_next` 里跳过本核的任务——会破坏单核 `yield` 的 `next==prev` 直接返回。
- **别**让 `on_cpu` 偏移漂移——`context_switch.S` 硬编码了 `96(%rdi)`,改 `CpuContext` 布局必须同步改汇编 + `static_assert` 会拦。
- **别** `cmake --build build`(ALL)验证本章——host 单测既有债;用 `big_kernel_test` + `run-kernel-test`。
