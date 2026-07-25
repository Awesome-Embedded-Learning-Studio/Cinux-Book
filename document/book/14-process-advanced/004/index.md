---
title: 004 · 调度类与 SIGSTOP/CONT
---

# 004 · 调度类与 SIGSTOP/CONT

> 两个目标:把调度器从"写死的一种轮询"改成可插拔的调度策略(调度类)并加上优先级;兑现 047 留的债——让 SIGSTOP/CONT 真起调度效果。

## 本章路线

- [01 · 让调度策略可插拔,让 SIGSTOP/CONT 真起作用](01-scheduler-class.md)
