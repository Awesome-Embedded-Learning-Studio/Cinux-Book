---
title: SMP 多核（v1.0.0 弧）
---

# SMP 多核（v1.0.0 回迁弧）

> F4 SMP 弧:v1.0.0 最大差异化。ACPI 拓扑 → LAPIC/IOAPIC + PIC→APIC 切换 → per-CPU 架构(GS/swapgs)+ AP boot trampoline → 多核调度 → lockdep 锁序图。读法:`-smp 2` 双核 online 是这条弧的终点。
