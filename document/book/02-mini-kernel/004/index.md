---
title: 004 · 为大内核铺路
---

# 004 · 为大内核铺路

> 写出 ATA PIO 磁盘驱动和 ELF64 加载器,串成一条加载流水线;big kernel 本身要到 009 才登场,这一章用两个 demo 验证这套流水线能干活。

## 本章路线

- [01 · 导引:点亮什么、为什么需要它](01-intro.md)
- [02 · 设计图:磁盘布局、PIO 时序、ELF 流水线](02-design.md)
- [03 · 代码路线:ATA PIO、ELF 解析、load_elf、loader、demo](03-implementation.md)
- [04 · 调试现场](04-debug.md)
- [05 · 验证与下一站](05-wrapup.md)
