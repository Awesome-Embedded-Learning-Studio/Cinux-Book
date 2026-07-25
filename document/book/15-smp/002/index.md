---
title: 002 · per-CPU 架构与 LAPIC IPI
---

# 002 · per-CPU 架构与 LAPIC IPI

> 把 per-CPU 数据/结构从静态全局迁到 GS-based per-CPU 控制块(单核重构,为多核铺地基),并把 LAPIC 的 IPI(核间中断)接口立起来——BSP 唤醒 AP 的发令枪(INIT-SIPI-SIPI 协议的 BSP 侧)。

## 本章路线

- [01 · per-CPU 架构与 LAPIC IPI:给第二个核备好发令枪](01-percpu-ipi.md)
