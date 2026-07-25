---
title: 15 · SMP 多核
---

# 15 · SMP 多核

> SMP 弧:Cinux 多核这条线走到底的产物。ACPI 拓扑 → LAPIC/IOAPIC + PIC→APIC 切换 → per-CPU 架构(GS/swapgs)+ AP boot trampoline → 多核调度 → 锁序图与 race-detect 报警器 → deferred CoW 范式。读法:`-smp 2` 双核 online 是这条弧的终点。

## 阅读顺序

- [001 · ACPI 与 APIC](001/)
- [002 · per-CPU 架构与 LAPIC IPI](002/)
- [003 · AP boot trampoline 与多核调度](003/)
- [004 · 质量加固:SMP 真跑后清算并发债](004/)
- [005 · SMP 迁移竞态:一个抽函数的重构,踩出一个潜伏的 ctx 写花 bug](005/)
- [006 · SMP 竞态——从发现到根治](006/)
