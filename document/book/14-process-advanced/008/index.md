---
title: 008 · timer_queue 与 stats_kthread
---

# 008 · timer_queue 与 stats_kthread

> 084 章你用 poll 有限 timeout 真睡了一个任务——timer_queue_arm 内部到底挂了什么、谁负责到点叫醒、为什么不会和运行队列撞锁,那半边没讲。这一章把那半边翻给你看;同一章还有第二件基础设施 stats_kthread,围绕「不饿死、不扭曲、不垄断 CPU」三个角的不可能三角。

## 本章路线

- [01 · 导引:点亮什么](01-intro.md)
- [02 · timer_queue 的内部:固定表 + tick 扫过期](02-timer-queue.md)
- [03 · stats_kthread:周期采样的常驻 kthread](03-stats-kthread.md)
- [04 · band 0 kthread 不能 sti/hlt:yield 解法](04-yield.md)
- [05 · dump_memory_stats:四条正交维度 + PF delta](05-dump-memory-stats.md)
- [06 · 验证、没做的、小结](06-wrapup.md)
