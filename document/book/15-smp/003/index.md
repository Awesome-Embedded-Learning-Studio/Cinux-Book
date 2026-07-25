---
title: 003 · AP boot trampoline 与多核调度
---

# 003 · AP boot trampoline 与多核调度

> 把 IPI 发令枪真按下去:写一段 trampoline 把第二个核从 16 位实模式拉到 64 位长模式,再让多核调度真正干活——AP 不再干站着 halt,而是从共享 run queue 里拉任务跑。这条路中间有个藏得很深的迁移 GP,还有 AP 真跑线程之后才暴露出来的一批并发债。

## 本章路线

- [01 · 导引:点亮什么、为什么](01-intro.md)
- [02 · AP boot trampoline:把第二个核从实模式拉到长模式](02-trampoline.md)
- [03 · 多核调度:共享 run queue 与 reschedule IPI](03-scheduling.md)
- [04 · 迁移 GP 与并发债清算](04-debug.md)
- [05 · 收尾:已知局限、验证、下一站](05-wrapup.md)
