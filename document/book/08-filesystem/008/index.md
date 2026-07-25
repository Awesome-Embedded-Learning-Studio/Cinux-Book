---
title: 008 · 并发安全:给共享数据上锁
---

# 008 · 并发安全:给共享数据上锁

> 给内核里所有共享的可变状态(PMM、堆、调度队列、fd 表、文件偏移)上锁或改成原子,让它们在多个内核线程加时钟中断的并发轰炸下不再互相踩——一次全内核的并发一致性加固。

## 本章路线

- [01 · 导引:007 留下的雷有多响](01-intro.md)
- [02 · 同步原语:Spinlock、InterruptGuard、Mutex、Semaphore](02-primitives.md)
- [03 · 三层加固落地:谁加什么、PMM 怎么改、调用链](03-three-layers.md)
- [04 · 设计现场:调度器、release-before-block、键盘为什么关中断](04-design-notes.md)
- [05 · 收尾:验证与下一站](05-wrapup.md)
