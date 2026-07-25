---
title: 004 · SMAP 遇上 SMP —— 撤掉全局 stac,给用户内存访问配 exception table
---

# 004 · SMAP 遇上 SMP —— 撤掉全局 stac,给用户内存访问配 exception table

> 上一章入口级全局 stac 在 SMP 下是定时炸弹(RFLAGS.AC per-CPU、context_switch 不存它);这一章撤全局 stac、改局部 accessor,syscall 切两层,再给 accessor 配 exception table——两件事一起把雷拆了,顺手还掉上一章末尾的债。

## 本章路线

- [01 · 导引:全局 stac 在 SMP 下会丢](01-intro.md)
- [02 · 修法一:撤全局 stac,改局部 accessor](02-accessor.md)
- [03 · 修法一的载体:syscall 切两层](03-syscall.md)
- [04 · 修法二:给 accessor 配 exception table](04-extable.md)
- [05 · 验证、附带增量与小结](05-wrapup.md)
