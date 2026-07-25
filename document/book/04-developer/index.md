---
title: 04 · 开发者 / 可观测性
---

# 04 · 开发者 / 可观测性

> 内核自己用的开发/调试基建:frame pointer、kallsyms 符号查找、backtrace、统一 panic、内存汇总、验证矩阵、双时钟。横切基建,不属任何 feature 弧但所有弧都受益。读法:需要"崩了能看懂栈""CI 真测到了 SMP""内核有了 monotonic 与墙钟"时来这卷。

## 阅读顺序

- [001 · kallsyms 与 panic backtrace](001/)
- [002 · 验证基建:SMP 唤醒与首故障捕获](002/)
- [003 · HPET 与 RTC:给内核两种时间](003/)
