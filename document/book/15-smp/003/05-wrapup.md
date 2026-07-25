---
title: 05 · 收尾:已知局限、验证、下一站
---

# 收尾:已知局限、验证、下一站

## 已知局限

- **`-smp 2` 真机的 AHCI 竞态**:本机 QEMU 下,两个核跑起来后,生产镜像的 AHCI 驱动会在 `identify` 上踩一个时序竞态 panic(单核 `-smp 1` 的 AHCI 完全正常,`Read sector 0` 成功跑到 GUI)。这是 AHCI 驱动和 SMP 的交互问题,跟本章的 SMP 调度代码无关——AP boot(`[AP1] online`)和调度机制都工作。换 QEMU 版本或环境(别的 QEMU 版本、或单核配置)不复现,所以是环境相关的 heisenbug。真机的"两核端到端跑用户任务"演示在本机被它挡住,后面收。
- **上面那句 scheduler.hpp 过时注释**:待清理。

## 验证

```bash
# trampoline + AP 入口 + 调度多核改造 + IPI 都在
grep -rn 'ap_trampoline\|ap_main\|boot_aps\|wake_idle_ap\|kRescheduleIpiVector\|ap_idle_entry\|has_runnable_task' kernel/arch/x86_64/ kernel/proc/ | grep -vE '\.o:' | head
# 共享 run queue 的多核纪律 + per-CPU idle
grep -n 'remove_at_locked\|pick_next\|idle_tasks_\|setup_ap_idle' kernel/proc/roundrobin.cpp kernel/proc/scheduler.cpp | head
```

构建 + 单核测试(单核行为不变,是回归基线):

```bash
cmake -B build -S . -DCINUX_BUILD_TESTS=ON && cmake --build build -j$(nproc)
cmake --build build --target run-kernel-test
```

`-smp 2` 看两个核 online、AP 经 trampoline 上线:

```bash
cmake --build build --target run-smp   # 启动日志见 [AP1] online (apic_id=1)
```

端到端:启动日志里 `[SMP] INIT-SIPI-SIPI -> apic_id 1` → `[AP1] GS anchored` → `[AP1] online`,两个核都 online。调度机制的更细验证(共享队列不 double-pick、lost-wakeup 关窗、原子 refcount)靠单核测试里的 scheduler/sync 并发用例——它们就是为多核正确性写的。真机 `-smp 2` 跑用户任务的端到端演示,受上面那个 AHCI heisenbug 挡住,本机跑不到那一步。

## 小结与下一站

第二个核真跑起来了:trampoline 把它从实模式拉到长模式,per-CPU idle + 共享 run queue 让两个核分工干活又不撞车,reschedule IPI 把闲置的核拽起来,迁移 GP 那个致命顺序也根治了。AP 真跑线程顺手把并发债(原子 refcount、lockdep)清算了一遍。

但 `-smp 2` 真机还有个 AHCI 驱动的时序竞态没收(本机 QEMU 复现、单核正常)。那是下一站要收拾的——连同任何别的 SMP 真跑后才冒头的迁移竞态。
