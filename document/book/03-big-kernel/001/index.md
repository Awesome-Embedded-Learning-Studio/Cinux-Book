---
title: 001 · 大内核登场:mini kernel 交棒
---

# 001 · 大内核登场:mini kernel 交棒

> mini kernel 用升级过的加载器把 big kernel 从磁盘读进来、跳进它的入口;串口上那一行 `[BIG] Big kernel running @ 0x1000000`,就是整个 004–009 接力跑的终点信号。

## 本章路线

- [01 · 导引:点亮什么、为什么、设计图](01-intro.md)
- [02 · 代码路线:big kernel 树与两阶段加载](02-implementation.md)
- [03 · 调试现场、验证与下一站](03-wrapup.md)
