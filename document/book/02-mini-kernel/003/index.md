---
title: 003 · 第一次能被打断
---

# 003 · 第一次能被打断

> 给 mini kernel 装上自己的 GDT 和第一张 IDT,写出异常处理 stub,让它第一次能"被打断"、看一眼出了什么事、然后活着继续。

## 本章路线

- [01 · 导引:点亮什么、为什么需要它](01-intro.md)
- [02 · 设计图:GDT/IDT/ISR 协作](02-design.md)
- [03 · 代码路线:GDT、IDT、ISR stub、伪错误码、C handler、门类型](03-implementation.md)
- [04 · 调试现场](04-debug.md)
- [05 · 验证与下一站](05-wrapup.md)
