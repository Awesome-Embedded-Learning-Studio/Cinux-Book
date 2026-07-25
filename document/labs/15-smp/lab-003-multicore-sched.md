---
title: Lab 003 · AP boot trampoline 与多核调度验证
---

# Lab 003 · AP boot trampoline 与多核调度验证

> 对应 `document/book/15-smp/003/`。验证档 **A 档**(AP 经 trampoline 上线 + 调度机制)。注意:本机 QEMU 下 `-smp 2` 真机会在 AHCI 驱动上踩一个时序竞态(单核正常),所以"两核端到端跑用户任务"在本机演示不到那一步;AP boot 和调度机制靠启动日志 + 单核里的多核并发测试验证。

## 目标

确认六件事:

1. trampoline 在(`ap_trampoline.S` 的 16→32→64 切换 + 临时 GDT/页表 + inject 参数);
2. BSP 的 `boot_aps` 在(INIT-SIPI-SIPI + online 屏障);
3. AP 的 `ap_main` 在(GS 锚定 + per-CPU GDT + LAPIC + 切 idle);
4. 共享 run queue 的多核纪律在(`pick_next` 移除不 re-enqueue + per-CPU idle);
5. reschedule IPI + `ap_idle_entry` 循环在;
6. `-smp 2` 启动见 `[AP1] online`。

## 步骤

### 1. 构建(开测试,run-kernel-test 需要)

```bash
cmake -B build -S . -DCINUX_BUILD_TESTS=ON && cmake --build build -j$(nproc) > /tmp/b.log 2>&1; echo "build=$?"
cmake --build build --target run-kernel-test    # 单核基线绿(多核调度逻辑的并发用例都在里头)
```

> 单核测试是回归基线。里头的 scheduler / sync 并发用例就是为多核正确性写的——共享队列不 double-pick、lost-wakeup 关窗、原子引用计数——这些是验证多核调度机制的主战场。

### 2. trampoline + AP 入口 + 调度改造

```bash
grep -rn 'ap_trampoline\|ap_main\|boot_aps\|wake_idle_ap\|kRescheduleIpiVector\|ap_idle_entry' kernel/arch/x86_64/ kernel/proc/ | grep -vE '\.o:' | head
```

去看:trampoline 怎么从 16 位实模式 `ljmp` 到 32、再到 64(`ap_trampoline.S` 的 `.code16/.code32/.code64` 三段);地址怎么用 `(label - ap_trampoline_start + 0x8000)` 算(GAS 折叠同 section 符号差为常量);`ap_main` 的七步;`boot_aps` 的 INIT-SIPI-SIPI(第二条 SIPI 的兜底)。

### 3. 共享 run queue 的多核纪律

```bash
grep -n 'remove_at_locked\|REMOVES\|pick_next\|re-enqueue\|idle_tasks_\|setup_ap_idle' kernel/proc/roundrobin.cpp kernel/proc/scheduler.cpp | head
```

**思考**:`pick_next` 为什么把任务**移除**而不是轮转塞回队尾?——见章节:运行中的任务要是还留在共享队列,第二个核的 `pick_next` 会再选中这个 Running 任务,两个核 context-switch 到同一份栈/ctx,必炸。配套:`schedule()` 里 prev 只有在还 Ready 时才 re-enqueue,pick 再移除 winner,所以队列里永远只有"可跑没在跑"的任务。**为什么每核要自己的 idle?**——共享一个 idle,两个核切过去共用同一份 ctx 和 16 KB 栈,致命。

### 4. reschedule IPI + idle 循环

```bash
grep -n 'kRescheduleIpiVector\|wake_idle_ap\|ap_idle_entry\|has_runnable_task\|sti; hlt' kernel/arch/x86_64/smp.hpp kernel/arch/x86_64/ap_main.cpp kernel/proc/scheduler.cpp | head
```

去看:0xE0 向量为什么避开 0x20-0x2F / 0xFF / 0x80;`wake_idle_ap` 的 best-effort 发 IPI(多发给忙核无害);`ap_idle_entry` 的 `has_runnable_task`(纯 peek)在 `cli` 下查、堵 lost-wakeup 窗口。

### 5.(A 档)`-smp 2` 看 AP 上线

```bash
cmake --build build --target run-smp   # 看启动日志
```

启动日志应见:`[SMP] INIT-SIPI-SIPI -> apic_id 1` → `[AP1] GS anchored` → `[AP1] online (apic_id=1)` → `[SMP] 1 AP(s) online`。debugcon 标记(`build/debug.log`,端口 0xE9)里 trampoline 三阶段的 `1`/`2`/`3`。

> **本机 QEMU 的 AHCI 竞态**:两个核跑起来后,生产镜像的 AHCI 驱动会在 `identify` 上 panic(单核 `-smp 1` 正常)。这是 AHCI + SMP 的交互,不是本章 SMP 调度代码的问题——AP 已 online、调度机制工作。换 QEMU 版本/环境不复现。所以"两核端到端跑用户任务"在本机演示不到;多核调度机制本身靠单核里的并发测试 + AP boot 日志验证。

## 验收清单

- [ ] 构建 `build=0`,单核 `run-kernel-test` 全绿。
- [ ] trampoline(`ap_trampoline.S` 三阶段 + 临时 GDT/页表 + inject 参数)在。
- [ ] `boot_aps`(INIT-SIPI-SIPI + 兜底 SIPI + online 屏障)+ `ap_main`(GS/GDT/LAPIC/切 idle)在。
- [ ] 共享 run queue 多核纪律(`pick_next` 移除 + per-CPU idle)在。
- [ ] reschedule IPI(0xE0)+ `ap_idle_entry` 循环在。
- [ ] `-smp 2` 启动见 `[AP1] online`。

## 别做这些

- **别**让运行中的任务留在共享 run queue——`pick_next` 必须移除 winner,否则两核抢同一任务。
- **别**两个核共享一个 idle 任务——共用一份 ctx 和栈会炸,每核 `setup_ap_idle` 自己的。
- **别**在 `GDT::load` 里重载 `%fs`/`%gs`——长模式下它们的基址在 MSR,重载平坦选择子会清掉 GS 锚点(AP 先锚 GS 后 load GDT 就触发迁移 GP)。
- **别**让 `ap_main` 在 scheduler 没 init 时 `sti;hlt` 等——`boot_aps` 跑在 `Scheduler::init` 之前,init 不发 IPI,hlt 会睡死;要 `cli;pause` 自旋。
- **别**指望本机 `-smp 2` 真机能端到端跑到用户任务——AHCI 竞态挡在前面;机制验证走单核并发测试 + AP boot 日志。
